#include <Arduino.h>
#include <math.h>
#include <new>
#include <ESP8266WiFi.h>
#include <ESP8266WebServer.h>
#include <Wire.h>
#include <flash_hal.h>

// Credentials stay in a local, Git-ignored header. Fresh checkouts still
// compile and provide the serial console with Wi-Fi disabled.
#if __has_include("wifi_config.h")
#include "wifi_config.h"
#else
#include "wifi_config.example.h"
#endif

struct Stage;
struct Job;
struct ScheduledCommand;
struct ClockWindow;

static const char *WIFI_SSID = COMMANDWARE_WIFI_SSID;
static const char *WIFI_PASSWORD = COMMANDWARE_WIFI_PASSWORD;
static const unsigned long WIFI_RETRY_INTERVAL = 15000;

static ESP8266WebServer webServer(80);
static bool webServerStarted = false;
static unsigned long lastWifiAttempt = 0;
static Print *commandOutput = &Serial;

class StringPrint : public Print
{
public:
    size_t write(uint8_t value) override
    {
        buffer += (char)value;
        return 1;
    }

    const String &str() const
    {
        return buffer;
    }

private:
    String buffer;
};

static void handleCommand(const String &line, uint16_t sourceLine = 1);
static void printJobs();
static bool scriptRequest(const String &command, const String *source, String &response);
static String trim(String value);

static const char WEB_PAGE[] PROGMEM = R"rawliteral(
<!doctype html>
<html lang="en">
<head>
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <meta charset="utf-8">
  <title>Commandware</title>
  <style>
    :root { color-scheme: dark; font: 14px/1.5 system-ui, sans-serif; background: #0b1120; color: #e5edf8; }
    * { box-sizing: border-box; }
    body { margin: 0; height: 100vh; height: 100dvh; display: flex; flex-direction: column; overflow: hidden; }
    header { flex: none; display: flex; align-items: baseline; gap: 1rem; padding: .7rem 1rem; border-bottom: 1px solid #26344a; }
    h1 { margin: 0; font-size: 1.25rem; letter-spacing: -.04em; }
    header p { margin: 0; color: #93a4bc; font-size: .85rem; }
    h2 { margin: 0; font-size: .95rem; }
    main { position: relative; display: grid; grid-template-columns: 20% 55% 25%; flex: 1; min-height: 0; }
    .panel { display: flex; flex-direction: column; min-width: 0; min-height: 0; background: #111b2d; }
    .panel + .panel { border-left: 1px solid #26344a; }
    .toolbar { display: flex; align-items: center; flex-wrap: wrap; gap: .5rem; min-height: 3.5rem; flex: none; padding: .65rem .85rem; border-bottom: 1px solid #26344a; }
    .toolbar h2, .toolbar label { margin-right: auto; font-weight: 650; }
    .scroll { flex: 1; min-height: 0; overflow: auto; overscroll-behavior: contain; padding: .75rem; }
    button, input { border: 1px solid #34445d; border-radius: .35rem; font: inherit; }
    button { padding: .35rem .7rem; background: #2563eb; border-color: #3b82f6; color: white; font-weight: 650; cursor: pointer; touch-action: manipulation; }
    button:hover { background: #1d4ed8; }
    button:disabled { opacity: .55; cursor: wait; }
    button:focus-visible, input:focus-visible, textarea:focus-visible, .divider:focus-visible { outline: 2px solid #93c5fd; outline-offset: -2px; }
    .secondary { background: #172338; border-color: #34445d; color: #c6d5e8; }
    #sample-filter { width: 100%; min-width: 0; padding: .5rem; background: #0b1424; color: #edf4ff; }
    .filter { flex: none; padding: .75rem .75rem 0; }
    #command-suggestions { display: grid; align-content: start; gap: .4rem; }
    #command-suggestions button { width: 100%; text-align: left; white-space: pre-wrap; overflow-wrap: anywhere; padding: .65rem; font: .85rem/1.5 ui-monospace, Consolas, monospace; background: #0b1424; border-color: #26344a; color: #c6d5e8; }
    #command-suggestions button:hover { background: #1b2e4a; border-color: #5683bd; }
    #command-suggestions h3 { display: inline; margin: 0; font-size: .8rem; color: #93a4bc; text-transform: uppercase; letter-spacing: .05em; }
    .sample-group summary { padding: .6rem .35rem; cursor: pointer; color: #93a4bc; border-bottom: 1px solid #26344a; }
    .sample-group summary:hover { background: #1b2e4a; }
    .sample-group summary:focus-visible { outline: 2px solid #93c5fd; }
    .sample-items { display: grid; gap: .4rem; padding-top: .4rem; }
    #command-suggestions strong, #command-suggestions small, #command-suggestions code { display: block; }
    #command-suggestions strong { font: 650 .88rem/1.4 system-ui, sans-serif; color: #edf4ff; }
    #command-suggestions small { margin: .3rem 0; font: .8rem/1.45 system-ui, sans-serif; color: #93a4bc; }
    #command-suggestions code { color: #93c5fd; }
    .sample-hint { margin: .4rem 0 0; color: #93a4bc; font-size: .8rem; }
    #command-form { margin: 0; }
    .editor-host { flex: 1; min-height: 0; min-width: 0; position: relative; overflow: hidden; background: #0b1424; }
    #command { display: block; width: 100%; height: 100%; margin: 0; padding: 1rem; resize: none; border: 0; border-radius: 0; background: #0b1424; color: #edf4ff; font: 14px/1.6 ui-monospace, Consolas, monospace; tab-size: 2; overflow: auto; }
    .editor-host .CodeMirror { height: 100%; background: #0b1424; color: #edf4ff; font: 14px/1.6 ui-monospace, Consolas, monospace; }
    .editor-host .CodeMirror-gutters { background: #101a2b; border-color: #26344a; }
    .editor-host .CodeMirror-linenumber { color: #73849d; }
    .editor-host .CodeMirror-cursor { border-left-color: #edf4ff; }
    .editor-host .CodeMirror-selected { background: #294369; }
    .editor-host .cm-keyword { color: #93c5fd; }
    .editor-host .cm-number { color: #fbbf24; }
    .editor-host .cm-string { color: #86efac; }
    .editor-host .cm-comment { color: #8495ad; }
    .editor-host .cm-variable-2 { color: #c4b5fd; }
    .editor-host .cm-operator { color: #f9a8d4; }
    .editor-status { padding: .35rem .85rem; border-top: 1px solid #26344a; color: #93a4bc; font-size: .8rem; }
    .script-bar { display: flex; flex-wrap: wrap; gap: .5rem 1rem; padding: .35rem .85rem; border-bottom: 1px solid #26344a; font-size: .8rem; }
    #script-title { overflow-wrap: anywhere; }
    #file-message { color: #93a4bc; }
    dialog { width: min(30rem, calc(100vw - 2rem)); max-height: 80vh; overflow: auto; padding: 1.25rem; border: 1px solid #34445d; border-radius: .7rem; background: #111b2d; color: #edf4ff; }
    dialog::backdrop { background: #0009; }
    dialog h2 { margin-bottom: .75rem; }
    dialog input { display: block; width: 100%; margin: .5rem 0 1rem; padding: .6rem; background: #0b1424; color: #edf4ff; }
    .dialog-actions { display: flex; justify-content: flex-end; gap: .5rem; margin-top: 1rem; }
    #saved-scripts { display: grid; gap: .5rem; }
    #saved-scripts button { text-align: left; overflow-wrap: anywhere; }
    #result { margin: 0 0 1rem; white-space: pre-wrap; overflow-wrap: anywhere; color: #c6d5e8; font: .9rem/1.6 ui-monospace, Consolas, monospace; }
    #jobs-heading { border-top: 1px solid #26344a; padding-top: .75rem; margin-bottom: .6rem; }
    #jobs-list { display: grid; gap: .5rem; margin: 0; padding: 0; list-style: none; }
    #jobs-list li { display: flex; align-items: center; justify-content: space-between; gap: .5rem; padding: .6rem; border: 1px solid #26344a; border-radius: .35rem; background: #0b1424; color: #c6d5e8; font-size: .85rem; overflow-wrap: anywhere; }
    #jobs-list button { flex: none; background: #451923; border-color: #7f3441; color: #fecdd3; font-size: .8rem; }
    .divider { position: absolute; top: 0; bottom: 0; width: 8px; transform: translateX(-50%); cursor: col-resize; touch-action: none; z-index: 5; }
    .divider:hover, .divider:focus-visible { background: #5683bd88; }
    #divider-left { left: 20%; } #divider-right { left: 75%; }
    @media (max-width: 600px) {
      header { gap: .5rem; padding: .5rem; } header p { display: none; }
      .toolbar { padding: .5rem; gap: .3rem; } .toolbar button { padding: .3rem; font-size: .8rem; }
      .filter, .scroll { padding: .4rem; } .editor-status { padding: .35rem; }
    }
  </style>
</head>
<body>
  <header><h1>Commandware</h1><p>Change your ESP8266 behavior with commands</p></header>
  <main id="workspace">
    <section class="panel" aria-labelledby="samples-heading">
      <div class="toolbar"><h2 id="samples-heading">Command samples</h2></div>
      <div class="filter"><input id="sample-filter" type="search" placeholder="Filter samples..." aria-label="Filter command samples"><p class="sample-hint">Click to insert at the cursor.</p></div>
      <div id="command-suggestions" class="scroll" aria-label="Command samples"></div>
    </section>
    <form id="command-form" class="panel" aria-label="Command editor">
      <div class="toolbar"><label for="command">Editor</label><button id="new-script" class="secondary" type="button">New</button><button id="open-script" class="secondary" type="button">Open</button><button id="save-script" class="secondary" type="button">Save</button><button id="run-command" type="submit" title="Run (Ctrl+Enter or Cmd+Enter)">Run</button></div>
      <div class="script-bar"><span id="script-title">Untitled</span><span id="file-message" role="status" aria-live="polite"></span></div>
      <div class="editor-host"><textarea id="command" name="command" autocomplete="off" autocapitalize="off" spellcheck="false" wrap="off" aria-label="Command editor" placeholder="Choose a sample or write commands here."></textarea></div>
      <div class="editor-status"><span id="editor-status" role="status">Basic editor · loading enhancements...</span> · Ctrl/Cmd+Enter to run</div>
    </form>
    <section class="panel" aria-labelledby="output-heading">
      <div class="toolbar"><h2 id="output-heading">Output</h2><button id="clear-output" class="secondary" type="button">Clear</button></div>
      <div class="scroll"><pre id="result" aria-live="polite">Choose a sample or write a command, then Run.</pre><h2 id="jobs-heading">Jobs and failure history</h2><p class="sample-hint">Failed entries are retained until reset, even after a later run succeeds.</p><ul id="jobs-list"><li>Loading...</li></ul></div>
    </section>
    <div id="divider-left" class="divider" role="separator" tabindex="0" aria-label="Resize samples and editor" aria-orientation="vertical" aria-valuemin="10" aria-valuemax="60" aria-valuenow="20"></div>
    <div id="divider-right" class="divider" role="separator" tabindex="0" aria-label="Resize editor and output" aria-orientation="vertical" aria-valuemin="35" aria-valuemax="90" aria-valuenow="75"></div>
  </main>
  <dialog id="save-dialog" aria-labelledby="save-heading">
    <form id="save-form">
      <h2 id="save-heading">Save script</h2>
      <label for="script-name">Script name</label><input id="script-name" maxlength="28" required autocomplete="off" placeholder="e.g. OLED sensor chart">
      <p>Saved on this device in flash memory.</p>
      <p id="save-error" role="alert"></p>
      <div class="dialog-actions"><button id="cancel-save" class="secondary" type="button">Cancel</button><button type="submit">Save script</button></div>
    </form>
  </dialog>
  <dialog id="open-dialog" aria-labelledby="open-heading">
    <h2 id="open-heading">Open saved script</h2>
    <div id="saved-scripts"></div>
    <div class="dialog-actions"><button id="cancel-open" class="secondary" type="button">Cancel</button></div>
  </dialog>
  <script>
    const f=document.getElementById('command-form'),i=document.getElementById('command'),r=document.getElementById('result'),d=document.getElementById('command-suggestions'),a=document.getElementById('new-script'),j=document.getElementById('jobs-list'),filter=document.getElementById('sample-filter'),run=document.getElementById('run-command'),status=document.getElementById('editor-status');
    const p=[
      {group:'Device and jobs',items:[
        ['Command reference','List supported commands and options.','help'],
        ['Device status','Inspect the board and active outputs.','status'],
        ['Pin availability','Check GPIO ownership before connecting a display or starting a job.','pins'],
        ['List jobs','Inspect running jobs, queued commands, and script failures.','jobs'],
        ['Stop one job','Replace 1 with the job ID shown in the output column.','kill 1'],
        ['Stop all jobs','Stop background work and clear queued commands; the display image remains.','reset']
      ]},
      {group:'OLED setup',items:['ssd1306','sh1106'].flatMap(controller=>[64,32].flatMap(height=>['0x3c','0x3d'].map(address=>[
        `${controller.toUpperCase()} 128×${height} · ${address}`,
        'SDA=GPIO4, SCL=GPIO5. Choose your controller, height, and I2C address. Initialization clears the screen.',
        `oled init ${controller} --width 128 --height ${height} --address ${address}`
      ])))},
      {group:'OLED text',items:[
        ['Hello OLED','Print at the top left. Uses current setup, or SSD1306 128×64 at 0x3c by default.','oled print "Hello, Commandware!"'],
        ['Positioned text','Place text at pixel x=2, y=16 using positional coordinates.','oled print 2 16 "Ready"'],
        ['Named coordinates','Place text using --x and --y options.','oled print "Ready" --x 2 --y 16'],
        ['Two lines of text','Clear old content and draw two rows. Works with 32- and 64-pixel displays.','do {\n  oled clear\n  oled print "Commandware"\n  oled print 0 8 "Ready to run"\n}'],
        ['ADC reading on OLED','Read A0 once and print its raw count on the second row.','adc A0 | oled print 0 8'],
        ['Live ADC text','Update every 200 ms. Stop its job in the output column; uses current OLED setup.','do {\n  oled clear\n  oled print "A0 counts"\n  every 200 | adc A0 | oled print 0 8\n}'],
        ['Clear OLED','Erase the display and chart histories; running jobs can draw again.','oled clear']
      ]},
      {group:'OLED charts',items:[
        ['Live ADC chart · 64px','Raw A0 counts, 200 ms per sample. Requires a 128×64 display; uses current controller/address.','do {\n  oled clear\n  oled print "A0 counts"\n  every 200 | adc A0 | oled chart --min 0 --max 1023 --y 8 --height 56\n}'],
        ['Live ADC chart · 32px','Compact trace for a 128×32 display. Initialize the correct height first.','do {\n  oled clear\n  oled print "A0 counts"\n  every 200 | adc A0 | oled chart --min 0 --max 1023 --y 8 --height 24\n}'],
        ['Live percentage chart','Map A0 counts to 0–100%. Requires a 128×64 display; uses current OLED setup.','do {\n  oled clear\n  oled print "A0 percent"\n  every 200 | adc A0 | map 0 1023 0 100 | oled chart --min 0 --max 100 --y 8 --height 56\n}'],
        ['Two charts from one reading','Raw counts and percent in separate areas on a 128×64 display. Stop old display jobs first.','do {\n  oled clear\n  oled print "Raw A0"\n  oled print 0 32 "Percent A0"\n  every 200 |\n    adc A0 --out raw |\n    map 0 1023 0 100 --out percent |\n    oled chart $raw --min 0 --max 1023 --y 8 --height 24 |\n    oled chart $percent --min 0 --max 100 --y 40 --height 24\n}']
      ]},
      {group:'GPIO and sensors',items:[
        ['Read GPIO12','Read a digital input; GPIO numbers are used, not board D labels.','read 12 | print'],
        ['Set GPIO12 high','Drive a connected output high. Leaves GPIO4/5 free for I2C.','write 12 1'],
        ['Set GPIO12 low','Drive a connected output low.','write 12 0'],
        ['100 ms output pulse','Drive GPIO12 high briefly, then return it low.','pulse 100 | write 12'],
        ['Blink every 500 ms','Toggle GPIO12 in a background job. Stop it with the Kill button.','every 500 | toggle | write 12'],
        ['Read analog input','Read the raw A0 count, from 0 to 1023.','adc A0 | print'],
        ['Analog percentage','Convert one A0 reading to a 0–100% value.','adc A0 | map 0 1023 0 100 | print'],
        ['Resistance measurement','Uses the calibrated resistor-divider circuit described in the tutorial.','adc A0 | ohm | print']
      ]},
      {group:'Scripts and sound',items:[
        ['Variables and arithmetic','Declare a value, change it, and print the result.','do {\n  set count 10\n  set count $count + 5\n  print $count\n}'],
        ['Reusable function','Define and call a function with a text parameter.','func greet name {\n  print "Hello $name"\n}\n\ndo {\n  greet "Commandware"\n}'],
        ['Two-note melody','Play C4 then E4 on a buzzer connected to GPIO15.','do {\n  tone 15 --hz 262 --dur 200\n  sleep 100\n  tone 15 --hz 330 --dur 300\n}'],
        ['Timed clock output','Generate 1 kHz on GPIO15 for 500 ms.','clock 1000 --dur 500 | write 15'],
        ['Schedule two notes','Start timed GPIO15 clock jobs 1 s and 1.5 s after submission.','schedule {\n  1000 { clock 1000 --dur 500 | write 15 }\n  1500 { clock 1400 --dur 1000 | write 15 }\n}']
      ]}
    ];
    let busy=false,editor=null,executing=false,currentName=null,savedValue='',fileBusy=false;
    const scriptTitle=document.getElementById('script-title'),fileMessage=document.getElementById('file-message'),saveDialog=document.getElementById('save-dialog'),openDialog=document.getElementById('open-dialog'),scriptName=document.getElementById('script-name');
    function editorValue(){return editor?editor.getValue():i.value}
    function updateScriptState(){const dirty=editorValue()!==savedValue;scriptTitle.textContent=(currentName||'Untitled')+(dirty?' · Unsaved changes':'');if(dirty)fileMessage.textContent=''}
    function setEditorValue(value){if(editor){editor.setValue(value);editor.clearHistory();editor.focus()}else{i.value=value;i.setSelectionRange(0,0);i.focus()}updateScriptState()}
    function discardChanges(){return editorValue()===savedValue||confirm('Discard unsaved changes to '+(currentName||'Untitled')+'?')}
    function validScriptName(name){return name.length<=28&&/^[A-Za-z0-9_ -]+$/.test(name)&&name===name.trim()}
    function scriptCommand(action,name,source){
      const command='script '+action+(name===undefined?'':' '+JSON.stringify(name));
      return sendScript(command,source);
    }
    async function saveScript(name){
      name=name.trim();
      const fail=message=>{fileMessage.textContent=message;document.getElementById('save-error').textContent=message;return false};
      if(!validScriptName(name))return fail('Use 1–28 letters, digits, spaces, _ or -.');
      if(fileBusy)return false;fileBusy=true;
      const source=editorValue();
      try{
        const names=await scriptCommand('list');
        if(names.includes(name)&&name!==currentName&&!confirm('Replace saved script "'+name+'"?'))return false;
        await scriptCommand('save',name,source);
        currentName=name;savedValue=source;updateScriptState();
        if(editorValue()===source)fileMessage.textContent='Saved on device.';
        return true;
      }catch(e){return fail('Unable to save: '+e.message+'. Your editor text is unchanged.')}
      finally{fileBusy=false}
    }
    async function openScripts(){
      if(fileBusy)return;fileBusy=true;
      const list=document.getElementById('saved-scripts');list.replaceChildren();
      try{
        const names=await scriptCommand('list');
        if(!names.length)list.textContent='No saved scripts yet. Use Save to name and store a script.';
        for(const name of names){
          const button=document.createElement('button');button.type='button';button.className='secondary';button.textContent=name;
          button.onclick=async()=>{
            if(fileBusy)return;fileBusy=true;
            try{
              const source=await scriptCommand('load',name);
              if(!openDialog.open||!discardChanges())return;
              currentName=name;savedValue=source;openDialog.close();setEditorValue(source);fileMessage.textContent='Opened from device.';
            }catch(e){fileMessage.textContent='Unable to open: '+e.message}
            finally{fileBusy=false}
          };list.append(button);
        }
        openDialog.showModal();
      }catch(e){fileMessage.textContent='Unable to read saved scripts: '+e.message}
      finally{fileBusy=false}
    }
    function insertSample(value){
      const content=editorValue(),cursor=editor?editor.indexFromPos(editor.getCursor('head')):i.selectionEnd;
      // Keep complete commands separate without deleting even a selected range.
      const text=(cursor>0&&content[cursor-1]!=='\n'?'\n':'')+value+(cursor<content.length&&content[cursor]!=='\n'?'\n':'');
      if(editor){const position=editor.posFromIndex(cursor);editor.replaceRange(text,position,position,'+input');editor.setCursor(editor.posFromIndex(cursor+text.length));editor.focus()}
      else{i.setRangeText(text,cursor,cursor,'end');i.focus()}
      updateScriptState();
    }
    const expandedGroups=new Set();
    function suggestions(){
      const query=filter.value.trim().toLowerCase();
      d.replaceChildren();
      for(const group of p){
        const matches=group.items.filter(item=>[group.group,...item].join(' ').toLowerCase().includes(query));
        if(!matches.length)continue;
        const section=document.createElement('details'),summary=document.createElement('summary'),heading=document.createElement('h3'),items=document.createElement('div');
        section.className='sample-group';section.open=Boolean(query)||expandedGroups.has(group.group);
        heading.textContent=group.group;summary.append(heading);items.className='sample-items';
        summary.onclick=e=>{e.preventDefault();section.open=!section.open;if(!query){if(section.open)expandedGroups.add(group.group);else expandedGroups.delete(group.group)}};
        section.append(summary,items);d.append(section);
        for(const [title,description,code] of matches){
          const b=document.createElement('button'),name=document.createElement('strong'),detail=document.createElement('small'),preview=document.createElement('code');
          b.type='button';b.title='Insert at cursor';b.setAttribute('aria-label','Insert '+title);
          name.textContent=title;detail.textContent=description;
          const lines=code.split('\n');preview.textContent=lines[0]+(lines.length>1?` … (${lines.length} lines)`:'');
          b.append(name,detail,preview);b.onclick=()=>insertSample(code);items.append(b);
        }
      }
      if(!d.children.length)d.textContent='No matching commands.';
      d.scrollTop=0;
    }
    let requestQueue=Promise.resolve();
    function request(path,options){const pending=requestQueue.then(async()=>{const controller=new AbortController(),timer=setTimeout(()=>controller.abort(),10000);try{const x=await fetch(path,{...options,signal:controller.signal}),t=await x.text();if(!x.ok)throw Error(t||x.statusText);return t}catch(e){if(e.name==='AbortError')throw Error('Request timed out. Check device connection; command may have executed.');throw e}finally{clearTimeout(timer)}});requestQueue=pending.catch(()=>{});return pending}
    function send(c){return request('/api/command',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams({command:c})})}
    async function sendScript(command,source){
      const fields={command,result:'script'};
      if(source!==undefined)fields.source=source;
      let text;
      try{text=await request('/api/command',{method:'POST',headers:{'Content-Type':'application/x-www-form-urlencoded'},body:new URLSearchParams(fields)})}
      catch(e){let failure;try{failure=JSON.parse(e.message)}catch{}throw Error(failure?.error||e.message)}
      const result=JSON.parse(text);
      if(!result.ok)throw Error(result.error);
      return result.value;
    }
    function getJobs(){return request('/api/jobs',{method:'GET',cache:'no-store'})}
    async function refresh(){
      if(busy)return;busy=true;
      try{
        const text=await getJobs(),entries=text.split(/\r?\n/).map(s=>s.match(/^(\d+)\s+(RUNNING|FAILED)\s+(.*)$/)).filter(Boolean);
        j.replaceChildren();
        if(!entries.length){j.textContent='No active jobs or recorded failures.';return}
        entries.forEach(m=>{
          const li=document.createElement('li');
          li.append(document.createTextNode(`#${m[1]} ${m[2]==='FAILED'?'FAILED (history)':'RUNNING'} ${m[3]} `));
          if(m[2]==='RUNNING'){
            const b=document.createElement('button');b.textContent='Kill';
            b.onclick=async()=>{b.disabled=true;try{r.textContent=await send(`kill ${m[1]}`);await refresh()}catch(e){r.textContent=`Request failed: ${e}`;b.disabled=false}};
            li.append(b);
          }
          j.append(li);
        });
      }catch(e){j.textContent=`Unable to load jobs: ${e}`}finally{busy=false}
    }
    a.onclick=()=>{if(fileBusy||!discardChanges())return;currentName=null;savedValue='';setEditorValue('');fileMessage.textContent=''};
    document.getElementById('open-script').onclick=openScripts;
    document.getElementById('save-script').onclick=()=>{if(fileBusy)return;if(currentName!==null){saveScript(currentName);return}scriptName.value='';document.getElementById('save-error').textContent='';saveDialog.showModal();scriptName.focus()};
    document.getElementById('save-form').onsubmit=async e=>{e.preventDefault();if(await saveScript(scriptName.value))saveDialog.close()};
    document.getElementById('cancel-save').onclick=()=>saveDialog.close();
    document.getElementById('cancel-open').onclick=()=>openDialog.close();
    i.addEventListener('input',updateScriptState);
    filter.oninput=suggestions;
    document.getElementById('clear-output').onclick=()=>{r.textContent=''};
    f.onsubmit=async e=>{
      e.preventDefault();
      const c=editorValue().trim();
      if(!c||executing)return;
      executing=true;run.disabled=true;r.textContent='Running...';
      try{r.textContent=await send(c);await refresh()}
      catch(x){r.textContent=`Request failed: ${x}`}
      finally{executing=false;run.disabled=false}
    };
    f.addEventListener('keydown',e=>{
      if((e.ctrlKey||e.metaKey)&&e.key==='Enter'){e.preventDefault();f.requestSubmit()}
      if(!editor&&e.target===i&&e.key==='Tab'){e.preventDefault();i.setRangeText('  ',i.selectionStart,i.selectionEnd,'end');updateScriptState()}
    });
    const workspace=document.getElementById('workspace'),left=document.getElementById('divider-left'),right=document.getElementById('divider-right');
    let sizes=[20,55,25];
    function resizePanels(handle,position){
      let boundary;
      if(handle===left){boundary=Math.max(10,Math.min(100-sizes[2]-25,position));sizes=[boundary,100-sizes[2]-boundary,sizes[2]]}
      else{boundary=Math.max(sizes[0]+25,Math.min(90,position));sizes=[sizes[0],boundary-sizes[0],100-boundary]}
      workspace.style.gridTemplateColumns=sizes.map(n=>n+'%').join(' ');
      left.style.left=sizes[0]+'%';right.style.left=(100-sizes[2])+'%';
      left.setAttribute('aria-valuenow',Math.round(sizes[0]));left.setAttribute('aria-valuemax',Math.round(100-sizes[2]-25));
      right.setAttribute('aria-valuenow',Math.round(100-sizes[2]));right.setAttribute('aria-valuemin',Math.round(sizes[0]+25));
      if(editor)editor.refresh();
    }
    [left,right].forEach(handle=>{
      handle.onpointerdown=e=>{
        if(e.button!==0)return;
        e.preventDefault();handle.setPointerCapture(e.pointerId);
        handle.onpointermove=event=>{const rect=workspace.getBoundingClientRect();resizePanels(handle,(event.clientX-rect.left)/rect.width*100)};
      };
      handle.onpointerup=handle.onpointercancel=handle.onlostpointercapture=()=>{handle.onpointermove=null};
      handle.onkeydown=e=>{if(e.key==='ArrowLeft'||e.key==='ArrowRight'){e.preventDefault();resizePanels(handle,(handle===left?sizes[0]:100-sizes[2])+(e.key==='ArrowLeft'?-2:2))}};
    });
    async function loadEditor(){
      const base='https://cdn.jsdelivr.net/npm/codemirror@5.65.21/lib/';
      let script,css,timer;
      try{
        await new Promise((resolve,reject)=>{
          timer=setTimeout(()=>reject(Error('CDN timeout')),6000);
          css=document.createElement('link');css.rel='stylesheet';css.href=base+'codemirror.css';
          script=document.createElement('script');script.src=base+'codemirror.js';script.async=true;
          let loaded=0;const ready=()=>{if(++loaded===2)resolve()};
          css.onload=script.onload=ready;css.onerror=script.onerror=()=>reject(Error('CDN unavailable'));
          document.head.append(css,script);
        });
        CodeMirror.defineMode('commandware',()=>({
          token(stream){
            if(stream.eatSpace())return null;
            if(stream.match(/^#.*/))return 'comment';
            if(stream.match(/^"(?:[^"\\]|\\.)*"|^'(?:[^'\\]|\\.)*'/))return 'string';
            if(stream.match(/^\$[\w]+/))return 'variable-2';
            if(stream.match(/^\b\d+(?:\.\d+)?\b/))return 'number';
            if(stream.match(/^(?:--[\w-]+|&&|\|\||==|!=|<=|>=|[|{}()+*/%!<>=-])/))return 'operator';
            if(stream.match(/^[a-zA-Z_]\w*/))return 'keyword';
            stream.next();return null;
          }
        }));
        const cursor=i.selectionEnd;
        editor=CodeMirror.fromTextArea(i,{lineNumbers:true,mode:'commandware',theme:'commandware',indentUnit:2,tabSize:2,lineWrapping:false,viewportMargin:10,extraKeys:{'Ctrl-Enter':()=>f.requestSubmit(),'Cmd-Enter':()=>f.requestSubmit(),Tab:cm=>cm.somethingSelected()?cm.indentSelection('add'):cm.replaceSelection('  ')}});
        editor.setCursor(editor.posFromIndex(cursor));
        editor.on('change',updateScriptState);
        editor.getInputField().setAttribute('aria-label','Command editor');
        status.textContent='CodeMirror editor';
      }catch(e){
        if(editor){editor.toTextArea();editor=null}
        if(script)script.remove();if(css)css.remove();
        status.textContent='Basic editor · CDN unavailable';
      }finally{clearTimeout(timer)}
    }
    suggestions();refresh();setInterval(refresh,3000);loadEditor();
  </script>
</body>
</html>
)rawliteral";

static void configureWebServer()
{
    webServer.on("/", HTTP_GET, []()
                 { webServer.send_P(200, "text/html; charset=utf-8", WEB_PAGE); });
    webServer.on("/api/jobs", HTTP_GET, []()
                 {
                     StringPrint response;
                     Print *previousOutput = commandOutput;
                     commandOutput = &response;
                     printJobs();
                     commandOutput = previousOutput;
                     webServer.sendHeader("Cache-Control", "no-store");
                     webServer.send(200, "text/plain; charset=utf-8", response.str()); });
    webServer.on("/api/command", HTTP_POST, []()
                 {
                     String command = webServer.arg("command");
                     if (webServer.arg("result") == "script") {
                         String source = webServer.arg("source"), response;
                         bool ok = scriptRequest(command, webServer.hasArg("source") ? &source : nullptr, response);
                         webServer.sendHeader("Cache-Control", "no-store");
                         // Structured failures use the same envelope as successes.
                         webServer.send(ok ? 200 : 400, "application/json; charset=utf-8", response);
                         return;
                     }
                     if (trim(command).length() == 0)
                     {
                         webServer.send(400, "text/plain; charset=utf-8", "ERROR: command is required at line 1");
                         return;
                     }

                     StringPrint response;
                     Print *previousOutput = commandOutput;
                     commandOutput = &response;
                     handleCommand(command);
                     commandOutput = previousOutput;

                     int statusCode = response.str().startsWith("ERROR:") ? 400 : 200;
                     webServer.send(statusCode, "text/plain; charset=utf-8", response.str()); });
    webServer.onNotFound([]()
                         { webServer.send(404, "text/plain; charset=utf-8", "Not found"); });
}

static void serviceWebServer()
{
    if (WIFI_SSID[0] == '\0')
    {
        return;
    }

    if (WiFi.status() == WL_CONNECTED)
    {
        if (!webServerStarted)
        {
            webServer.begin();
            webServerStarted = true;
            Serial.print(F("Web interface: http://"));
            Serial.println(WiFi.localIP());
        }
        webServer.handleClient();
        return;
    }

    unsigned long now = millis();
    if (now - lastWifiAttempt >= WIFI_RETRY_INTERVAL)
    {
        lastWifiAttempt = now;
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        Serial.println(F("Reconnecting to Wi-Fi..."));
    }
}

#include "ScriptRuntime.h"
#include "ScriptStorage.h"
static cw::ScriptStorage scriptStorage;
#include "OledDisplay.h"
static cw::OledDisplay oledDisplay;
// Hardware/transport errors use the same source locations as runtime errors.
static uint16_t commandErrorLine = 1;
class ErrorLinePrint : public Print
{
    Print &destination;
    String line;
public:
    explicit ErrorLinePrint(Print &target) : destination(target) {}
    size_t write(uint8_t value) override
    {
        if (value == '\n') flushLine(true);
        else if (value != '\r') line += (char)value;
        return 1;
    }
    void flushLine(bool newline = false)
    {
        destination.print(line);
        if (line.startsWith("ERROR:") && line.indexOf(" at line ") < 0)
        {
            destination.print(F(" at line "));
            destination.print(commandErrorLine);
        }
        if (newline) destination.print('\n');
        line = "";
    }
};

class ErrorOutputScope
{
    Print *previous;
    uint16_t previousLine;
    ErrorLinePrint output;
public:
    explicit ErrorOutputScope(uint16_t sourceLine = 1)
        : previous(commandOutput), previousLine(commandErrorLine), output(*previous)
    { commandOutput = &output; commandErrorLine = sourceLine; }
    ~ErrorOutputScope()
    { output.flushLine(); commandOutput = previous; commandErrorLine = previousLine; }
};
static cw::Runtime scriptRuntime;
static std::map<int, unsigned long> scriptPinOwners;
static std::map<int, bool> scriptOutputPins;
static void printStorageStatus()
{
#if defined(ARDUINO_ARCH_ESP8266)
    commandOutput->print(F("Flash configured bytes: ")); commandOutput->println(ESP.getFlashChipSize());
    commandOutput->print(F("Flash detected bytes: ")); commandOutput->println(ESP.getFlashChipRealSize());
    commandOutput->print(F("LittleFS partition offset bytes: ")); commandOutput->println((unsigned long)FS_PHYS_ADDR);
    commandOutput->print(F("LittleFS partition size bytes: ")); commandOutput->println((unsigned long)FS_PHYS_SIZE);
#endif
    if (scriptStorage.mountError()) {
        commandOutput->print(F("LittleFS unavailable: ")); commandOutput->println(scriptStorage.mountError());
        commandOutput->println(F("Filesystem was not formatted. See README: provision-littlefs.sh."));
        return;
    }
    FSInfo info;
    if (!LittleFS.info(info)) { commandOutput->println(F("LittleFS mounted; capacity query failed")); return; }
    commandOutput->print(F("LittleFS ready; used bytes: ")); commandOutput->print(info.usedBytes);
    commandOutput->print(F("; total bytes: ")); commandOutput->println(info.totalBytes);
}
static void configureScriptRuntime();
static bool scriptOwnsPin(int pin) { return scriptPinOwners.count(pin) != 0; }
static bool displayReservesPin(int pin);

static unsigned long nextJobId = 1;

static String inputBuffer;
static int inputBraceDepth = 0;
static bool inputQuoted = false;
static bool inputEscaped = false;
static char inputQuoteChar = 0;
static bool inputComment = false;
static bool inputAfterCR = false;
static uint16_t inputLine = 1;

static String trim(String value)
{
    while (value.length() > 0 && (value[0] == ' ' || value[0] == '\t' || value[0] == '\r' || value[0] == '\n'))
    {
        value.remove(0, 1);
    }
    while (value.length() > 0 && (value[value.length() - 1] == ' ' || value[value.length() - 1] == '\t' || value[value.length() - 1] == '\r' || value[value.length() - 1] == '\n'))
    {
        value.remove(value.length() - 1, 1);
    }
    return value;
}

// Common PCF8574 backpack: P0=RS, P1=RW, P2=E, P3=backlight,
// P4..P7=D4..D7. Wire ships with the ESP8266 core.
static bool lcdReady = false;
static uint8_t lcdAddress = 0x27;
static bool displayReservesPin(int pin) { return (lcdReady || oledDisplay.reserved) && (pin == 4 || pin == 5); }

static bool lcdNibble(uint8_t value)
{
    Wire.beginTransmission(lcdAddress);
    Wire.write((uint8_t)(value | 0x08));
    Wire.write((uint8_t)(value | 0x0c));
    Wire.write((uint8_t)(value | 0x08));
    return Wire.endTransmission() == 0;
}

static bool lcdByte(uint8_t value, bool data = false)
{
    bool ok = lcdNibble((value & 0xf0) | (data ? 1 : 0)) &&
              lcdNibble((value << 4) | (data ? 1 : 0));
    delayMicroseconds(50);
    return ok;
}

static bool lcdFailure()
{
    lcdReady = false;
    commandOutput->println(F("ERROR: LCD I2C communication failed; check wiring and address"));
    return false;
}

static bool initLCD(uint8_t address)
{
    if (oledDisplay.reserved && address == oledDisplay.address) {
        commandOutput->println(F("ERROR: LCD address is already used by OLED")); return false;
    }
    if (scriptOwnsPin(4) || scriptOwnsPin(5))
    {
        commandOutput->println(F("ERROR: LCD pins are owned by a script job"));
        return false;
    }
    lcdReady = false;
    lcdAddress = address;
    Wire.begin(4, 5); // SDA, SCL: GPIO numbers, not board D labels.
    Wire.setClock(100000);
    Wire.beginTransmission(lcdAddress);
    if (Wire.endTransmission() != 0) return lcdFailure();
    // HD44780 power-on / 4-bit initialization (datasheet figure 24).
    delay(50);
    if (!lcdNibble(0x30)) return lcdFailure();
    delayMicroseconds(4500);
    if (!lcdNibble(0x30)) return lcdFailure();
    delayMicroseconds(150);
    if (!lcdNibble(0x30) || !lcdNibble(0x20) || !lcdByte(0x28) ||
        !lcdByte(0x08) || !lcdByte(0x01)) return lcdFailure();
    delay(2);
    if (!lcdByte(0x06) || !lcdByte(0x0c)) return lcdFailure();
    lcdReady = true;
    return true;
}

static bool printLCD(const String &text)
{
    if (!lcdReady && !initLCD(lcdAddress)) return false;
    char cells[32];
    for (int i = 0; i < 32; ++i) cells[i] = ' ';
    int pos = 0;
    for (unsigned int i = 0; i < text.length() && pos < 32; ++i)
        if (text[i] == '\n') pos = pos < 16 ? 16 : 32;
        else cells[pos++] = text[i];
    // Overwrite with spaces instead of clearing to avoid flicker and stale digits.
    for (int row = 0; row < 2; ++row)
    {
        if (!lcdByte(row ? 0xc0 : 0x80)) return lcdFailure();
        for (int col = 0; col < 16; ++col)
            if (!lcdByte(cells[row * 16 + col], true)) return lcdFailure();
    }
    return true;
}

static void printHelp()
{
    commandOutput->println(F("Commandware: events flow through source | transform | action"));
    commandOutput->println(F("read PIN [--out NAME]; adc A0 [--watch] [--out NAME]"));
    commandOutput->println(F("write PIN [VALUE | --value VALUE]; print [VALUE | --value VALUE]"));
    commandOutput->println(F("map IN_MIN IN_MAX OUT_MIN OUT_MAX [VALUE | --value VALUE] [--out NAME]"));
    commandOutput->println(F("not [VALUE | --value VALUE]; ohm [VALUE | --value VALUE]"));
    commandOutput->println(F("clock HZ [--dur MS] [--out NAME] | write PIN; every MS [--out NAME] | COMMAND"));
    commandOutput->println(F("edge; rising; falling; toggle; divide N; debounce MS; pulse MS"));
    commandOutput->println(F("sleep MS; tone PIN --hz HZ --dur MS (cooperative waits)"));
    commandOutput->println(F("set NAME EXPR; do { BODY }; if EXPR { BODY } [else { BODY }]; where EXPR"));
    commandOutput->println(F("func NAME [PARAM...] { BODY }; NAME [ARG...] or NAME --PARAM VALUE"));
    commandOutput->println(F("max 5 min 10 3; max --a 5 --b 10; parentheses override grouping"));
    commandOutput->println(F("number/int/string/abs/round/floor/ceil X; min/max A B; clamp X LOW HIGH"));
    commandOutput->println(F("len X; contains X ITEM; slice X START END; at ITEMS INDEX; join ITEMS SEPARATOR"));
    commandOutput->println(F("trim/lower/upper TEXT; replace TEXT OLD NEW; notehz TEXT"));
    commandOutput->println(F("split DELIMITER [VALUE | --value VALUE] [--out NAME,...]; each [--out NAME] { BODY }"));
    commandOutput->println(F("schedule { OFFSET_MS { BODY } ... }"));
    commandOutput->println(F("shiftin DATA CLOCK LOAD [msb|lsb] [--bits 1..53]"));
    commandOutput->println(F("shiftout DATA CLOCK [LATCH] [msb|lsb] [--bits 1..53] [--value VALUE]"));
    commandOutput->println(F("lcd init [ADDRESS]; lcd print [VALUE | --value VALUE]; lcd clear"));
    commandOutput->println(F("oled init [ssd1306|sh1106] [--height 32|64] [--address 0x3c|0x3d]"));
    commandOutput->println(F("oled print [X Y] [VALUE] [--x X --y Y]; oled chart [VALUE] [--min N --max N --x X --y Y --width W --height H]; oled rect X Y WIDTH HEIGHT [--value 0|1]; oled clear"));
    commandOutput->println(F("script list | print; script save NAME (string input); script load NAME | print; script delete NAME"));
    commandOutput->println(F("help [COMMAND]; pins; jobs; kill ID; status; reset (preserves GPIO and functions)"));
    commandOutput->println(F("Shift frames default to 8 bits. run NAME is simulator-only. Use print for console values."));
}

static void printPins()
{
    const char *names[] = {
        "GPIO0", "GPIO1", "GPIO2", "GPIO3", "GPIO4", "GPIO5",
        "GPIO12", "GPIO13", "GPIO14", "GPIO15", "GPIO16", "A0"};
    const uint8_t pins[] = {0, 1, 2, 3, 4, 5, 12, 13, 14, 15, 16, A0};
    for (int i = 0; i < 12; ++i)
    {
        commandOutput->print(names[i]);
        commandOutput->print("\t");
        if (pins[i] == 1 || pins[i] == 3)
        {
            commandOutput->println(F("terminal"));
        }
        else if (displayReservesPin(pins[i]))
        {
            commandOutput->println(oledDisplay.reserved ? F("OLED I2C") : F("LCD I2C"));
        }
        else if (scriptOwnsPin(pins[i]))
        {
            commandOutput->print(F("script job "));
            commandOutput->println(scriptPinOwners[pins[i]]);
        }
        else
        {
            commandOutput->println(F("available"));
        }
    }
}

static void printJobs()
{
    for (const auto &failure : scriptRuntime.failures)
    {
        commandOutput->print(failure.id);
        commandOutput->print(F(" FAILED  "));
        commandOutput->print(failure.label.c_str());
        commandOutput->print(F(": "));
        commandOutput->println(failure.message.c_str());
    }
    for (const auto &task : scriptRuntime.tasks)
    {
        commandOutput->print(task->id);
        commandOutput->print(F(" RUNNING  "));
        if (task->scheduled) commandOutput->print(F("[queued] "));
        commandOutput->println(task->label.c_str());
    }
}

static bool scriptClaimPin(int pin, unsigned long owner)
{
    // UART and flash pins are unavailable to script hardware commands.
    if (pin < 0 || pin > 16 || pin == 1 || pin == 3 || (pin >= 6 && pin <= 11)) return false;
    if (displayReservesPin(pin)) return false;
    auto existing = scriptPinOwners.find(pin);
    if (existing != scriptPinOwners.end()) return existing->second == owner;
    scriptPinOwners[pin] = owner;
    return true;
}

static void configureScriptRuntime()
{
    oledDisplay.begin = [](int address, std::string &error) {
        if (lcdReady && address == lcdAddress) { error = "oled: address is already used by LCD"; return false; }
        if (scriptOwnsPin(4) || scriptOwnsPin(5)) { error = "oled: I2C pins are owned by a script job"; return false; }
        Wire.begin(4, 5);
        Wire.setClock(100000);
        return true;
    };
    oledDisplay.send = [](uint8_t address, uint8_t control, const uint8_t *bytes, size_t count) {
        Wire.beginTransmission(address);
        if (Wire.write(control) != 1) { Wire.endTransmission(); return 1; }
        for (size_t i = 0; i < count; ++i) if (Wire.write(bytes[i]) != 1) { Wire.endTransmission(); return 1; }
        return (int)Wire.endTransmission();
    };
    scriptRuntime.newId = []() { return nextJobId++; };
    scriptRuntime.output = [](const std::string &s) { commandOutput->println(s.c_str()); };
    scriptRuntime.claim = scriptClaimPin;
    scriptRuntime.write = [](int pin, int value) {
        scriptOutputPins[pin] = true;
        pinMode(pin, OUTPUT); digitalWrite(pin, value ? HIGH : LOW);
    };
    scriptRuntime.release = [](unsigned long id) {
        for (auto it = scriptPinOwners.begin(); it != scriptPinOwners.end();)
            if (it->second == id) {
                scriptOutputPins.erase(it->first);
                it = scriptPinOwners.erase(it);
            } else ++it;
    };
    scriptRuntime.host = [](const std::string &name, const std::vector<cw::Value> &supplied, cw::Event &event,
                            unsigned long id, std::string &error) {
        ErrorOutputScope output(scriptRuntime.executingLine);
        if (name == "script") return scriptStorage.command(supplied, event, error);
        if (name == "oled") return oledDisplay.command(supplied, event, scriptRuntime.executingLine, (uint32_t)millis(), error);
        auto args = supplied;
        cw::Value input = event.value;
        bool explicitValue = false;
        for (size_t i = 0; i < args.size(); ++i) if (args[i].option && args[i].str() == "--value") {
            if (explicitValue || i + 2 != args.size()) { error = "--value requires one final value"; return false; }
            input = args[i+1]; args.resize(i); explicitValue = true; break;
        }
        auto number = [&](const cw::Value &v) { return cw::numericValue(v, error); };
        auto integer = [&](const cw::Value &v, double low, double high) {
            double n = number(v);
            if (error.empty() && (n != std::trunc(n) || n < low || n > high)) error = "integer argument is outside supported range";
            return error.empty() ? n : 0.0;
        };
        auto pin = [&](const cw::Value &v) {
            int p = (int)integer(v, 0, 16);
            if (error.empty() && !scriptClaimPin(p, id)) error = "GPIO is invalid, reserved, or owned by another job";
            return p;
        };
        auto selected = [&](size_t positional, bool required = true) {
            if (args.size() > positional) {
                if (explicitValue) error = "provide positional value or --value, not both";
                else input = args[positional];
            } else if (required && !explicitValue && !event.hasValue) error = "pipeline value is required";
            return input;
        };
        if (name == "adc" && args.size() == 1 && cw::lower(args[0].str()) == "a0" && !explicitValue) {
            event.value = cw::Value(analogRead(A0)); event.hasValue = true; return true;
        }
        if (name == "read" && args.size() == 1 && !explicitValue) {
            int p = pin(args[0]); if (!error.empty()) return false;
            pinMode(p, INPUT); event.value = cw::Value(digitalRead(p)); event.hasValue = true; return true;
        }
        if (name == "write" && (args.size() == 1 || args.size() == 2)) {
            double value = number(selected(1));
            int p = pin(args[0]); if (!error.empty()) return false;
            scriptOutputPins[p] = true; pinMode(p, OUTPUT); digitalWrite(p, value != 0 ? HIGH : LOW);
            return true;
        }
        if (name == "map" && (args.size() == 4 || args.size() == 5)) {
            double value = number(selected(4));
            double a = number(args[0]), b = number(args[1]), c = number(args[2]), d = number(args[3]);
            if (!error.empty()) return false;
            if (a == b) { error = "map input range cannot be zero"; return false; }
            double result = std::trunc(c + (value-a) * (d-c) / (b-a));
            if (!std::isfinite(result)) { error = "map result must be finite"; return false; }
            event.value = cw::Value(result); event.hasValue = true; return true;
        }
        if (name == "not" && args.size() <= 1) {
            double value = number(selected(0)); if (!error.empty()) return false;
            event.value = cw::Value(value == 0); event.hasValue = true; return true;
        }
        if (name == "print" && args.size() <= 1) {
            cw::Value value = selected(0); if (!error.empty()) return false;
            commandOutput->println(cw::formatValue(value).c_str()); return error.empty();
        }
        if (name == "lcd" && !args.empty()) {
            std::string action = cw::lower(args[0].str());
            if (action == "init" && args.size() <= 2 && !explicitValue) {
                int address = args.size() == 2 ? (int)integer(args[1], 8, 0x77) : 0x27;
                if (error.empty() && initLCD((uint8_t)address)) return true;
            } else if (action == "print" && args.size() <= 2) {
                cw::Value value = selected(1);
                std::string text = cw::formatValue(value);
                if (error.empty() && printLCD(String(text.c_str()))) return true;
            } else if (action == "clear" && args.size() == 1 && !explicitValue) {
                if (printLCD(String(""))) return true;
            }
            if (error.empty()) error = "LCD command failed";
            return false;
        }
        if (name == "ohm" && args.size() <= 1) {
            double rawValue = integer(selected(0), -2147483647.0, 2147483647.0);
            if (!error.empty()) return false;
            int raw = (int)rawValue;
            if (raw < 0 || raw > 1023) event.value = cw::Value(std::string("OUT_OF_RANGE"));
            else if (raw < 30) event.value = cw::Value(std::string("<270"));
            else if (raw > 720) event.value = cw::Value(std::string(">700k"));
            else {
                const int counts[] = {50,179,291,459,557,628,703,720};
                const double resistance[] = {270,995,1992,5152,9959,20020,108800,688900};
                int index = 0; while (index < 6 && raw > counts[index+1]) ++index;
                double fraction = double(raw-counts[index]) / (counts[index+1]-counts[index]);
                event.value = cw::Value(std::floor(resistance[index] * exp(fraction * log(resistance[index+1]/resistance[index])) + 0.5));
            }
            event.hasValue = true; return true;
        }
        if (name == "shiftin" || name == "shiftout") {
            bool reading = name == "shiftin";
            std::vector<cw::Value> pins;
            std::string order = "msb";
            int bits = 8; bool haveBits = false, haveOrder = false;
            for (size_t i = 0; i < args.size(); ++i) {
                if (args[i].option) {
                    if (args[i].str() != "--bits" || haveBits || i+1 == args.size()) { error = "invalid shift-register option"; return false; }
                    bits = (int)integer(args[++i], 1, 53); haveBits = true;
                } else if (args[i].kind == cw::Value::TEXT && (cw::lower(args[i].str()) == "msb" || cw::lower(args[i].str()) == "lsb")) {
                    if (haveOrder) { error = "duplicate bit order"; return false; }
                    order = cw::lower(args[i].str()); haveOrder = true;
                } else pins.push_back(args[i]);
            }
            if ((reading && (pins.size() != 3 || explicitValue)) || (!reading && (pins.size() < 2 || pins.size() > 3))) {
                error = "invalid shift-register pins/value"; return false;
            }
            double value = 0;
            if (!reading) {
                if (!explicitValue && !event.hasValue) { error = "pipeline value is required"; return false; }
                value = integer(input, 0, std::ldexp(1.0, bits)-1);
            }
            std::vector<int> gpioPins;
            for (const auto &v : pins) {
                int p = (int)integer(v, 0, 16);
                if (std::find(gpioPins.begin(), gpioPins.end(), p) != gpioPins.end()) error = "shift-register pins must be distinct";
                gpioPins.push_back(p);
            }
            if (!error.empty()) return false;
            for (int p : gpioPins) if (!scriptClaimPin(p, id)) { error = "GPIO is invalid, reserved, or owned by another job"; return false; }
            int data = gpioPins[0], clock = gpioPins[1];
            pinMode(clock, OUTPUT); digitalWrite(clock, LOW); scriptOutputPins[clock] = true;
            uint64_t frame = (uint64_t)value;
            if (reading) {
                int load = gpioPins[2]; pinMode(data, INPUT); pinMode(load, OUTPUT); scriptOutputPins[load] = true;
                digitalWrite(load, LOW); delayMicroseconds(1); digitalWrite(load, HIGH);
                frame = 0;
                for (int i = 0; i < bits; ++i) {
                    uint64_t sample = digitalRead(data) ? 1 : 0;
                    if (order == "msb") frame = (frame << 1) | sample;
                    else frame |= sample << i;
                    digitalWrite(clock, HIGH); delayMicroseconds(1); digitalWrite(clock, LOW);
                }
                event.value = cw::Value((double)frame); event.hasValue = true;
            } else {
                pinMode(data, OUTPUT); scriptOutputPins[data] = true;
                if (gpioPins.size() == 3) { pinMode(gpioPins[2], OUTPUT); scriptOutputPins[gpioPins[2]] = true; digitalWrite(gpioPins[2], LOW); }
                for (int i = 0; i < bits; ++i) {
                    int index = order == "msb" ? bits-1-i : i;
                    digitalWrite(data, (frame >> index) & 1); digitalWrite(clock, HIGH); delayMicroseconds(1); digitalWrite(clock, LOW);
                }
                if (gpioPins.size() == 3) digitalWrite(gpioPins[2], HIGH);
            }
            return true;
        }
        if (name == "help" && args.size() <= 1 && !explicitValue) { printHelp(); return true; }
        if (name == "pins" && args.empty() && !explicitValue) { printPins(); return true; }
        if (name == "jobs" && args.empty() && !explicitValue) { printJobs(); return true; }
        if (name == "kill" && args.size() == 1 && !explicitValue) {
            unsigned long target = (unsigned long)integer(args[0], 1, 4294967295.0);
            if (!error.empty()) return false;
            if (!scriptRuntime.kill(target)) { error = "unknown job ID"; return false; }
            commandOutput->println(F("killed")); return true;
        }
        if (name == "reset" && args.empty() && !explicitValue) {
            scriptRuntime.reset(); commandOutput->println(F("reset")); return true;
        }
        if (name == "status" && args.empty() && !explicitValue) {
            commandOutput->println(F("ESP8266 Commandware ready"));
            commandOutput->print(F("Uptime ms: ")); commandOutput->println(millis());
            commandOutput->print(F("Free heap: ")); commandOutput->println(ESP.getFreeHeap());
            commandOutput->print(F("Wi-Fi status: ")); commandOutput->println((int)WiFi.status());
            commandOutput->print(F("IP: ")); commandOutput->println(WiFi.localIP());
            printStorageStatus(); return true;
        }
        error = "invalid arguments for '" + name + "'"; return false;
    };
}

static void executeCommand(const String &line)
{
    if (trim(line).length() == 0) return;
    configureScriptRuntime();
    std::string source(commandErrorLine - 1, '\n');
    source += line.c_str();
    if (!scriptRuntime.submit(source, (uint32_t)millis(), (uint32_t)micros())) {
        commandOutput->print(F("ERROR: "));
        commandOutput->println(scriptRuntime.error.c_str());
    }
}

static void handleCommand(const String &line, uint16_t sourceLine)
{
    ErrorOutputScope output(sourceLine);
    executeCommand(line);
}

// JSON transport for a single synchronous script command. Other submissions
// retain their existing console response; source bytes never become code here.
static std::string jsonString(const std::string &value)
{
    std::string result = "\"";
    for (unsigned char c : value) {
        if (c == '"' || c == '\\') { result += '\\'; result += (char)c; }
        else if (c < 0x20) {
            char escape[7]; snprintf(escape, sizeof(escape), "\\u%04x", c); result += escape;
        } else result += (char)c;
    }
    return result + '"';
}

static bool scriptRequest(const String &command, const String *source, String &response)
{
    std::string error;
    cw::Parser parser(error);
    auto body = parser.parse(command.c_str());
    auto failure = [&](const std::string &message) {
        response = String(("{\"ok\":false,\"error\":" + jsonString(message) + "}").c_str());
        return false;
    };
    if (!error.empty()) return failure(error);
    if (body.size() != 1 || body[0].size() != 1 ||
        cw::lower(body[0][0]->name) != "script" || body[0][0]->hasBody)
        return failure("editor requests require a single script command");
    auto &args = body[0][0]->args;
    if (source && (args.empty() || cw::lower(args[0].text) != "save"))
        return failure("source is only accepted for script save");
    if (source && source->length() > 4096) return failure("script: source exceeds 4096 bytes");
    auto input = std::make_shared<cw::Event>(), result = std::make_shared<cw::Event>();
    if (source) { input->value = cw::Value(std::string(source->c_str(), source->length())); input->hasValue = true; }
    configureScriptRuntime();
    // Runtime console output is irrelevant to this response, including output
    // from already running jobs serviced by the same tick.
    class DiscardPrint : public Print { public: size_t write(uint8_t) override { return 1; } } console;
    Print *previous = commandOutput; commandOutput = &console;
    unsigned long id = nextJobId;
    bool returnsValue = !args.empty() && (cw::lower(args[0].text) == "list" || cw::lower(args[0].text) == "load");
    bool submitted = scriptRuntime.submit(command.c_str(), (uint32_t)millis(), (uint32_t)micros(), input, returnsValue ? result : nullptr);
    commandOutput = previous;
    if (!submitted) return failure(scriptRuntime.error);
    for (const auto &failed : scriptRuntime.failures) if (failed.id == id) return failure(failed.message);
    std::string value = "null";
    if (result->hasValue && result->value.kind == cw::Value::TEXT) value = jsonString(result->value.text);
    else if (result->hasValue && result->value.kind == cw::Value::LIST) {
        value = "[";
        for (size_t i = 0; i < result->value.list.size(); ++i) {
            if (i) value += ',';
            value += jsonString(result->value.list[i].text);
        }
        value += ']';
    }
    response = String(("{\"ok\":true,\"value\":" + value + "}").c_str());
    return true;
}

void setup()
{
    Serial.begin(115200);
    delay(1000);
    Serial.println(F("Commandware ESP-12E ready"));
    Serial.println(ESP.getResetReason());
    Serial.println(F("Type 'help' for commands"));

    scriptStorage.begin();
    printStorageStatus();
    configureWebServer();
    WiFi.mode(WIFI_STA);
    if (WIFI_SSID[0] != '\0')
    {
        WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
        lastWifiAttempt = millis();
        Serial.println(F("Connecting to Wi-Fi..."));
    }
    else
    {
        Serial.println(F("Set WIFI_SSID and WIFI_PASSWORD in the sketch to enable web access."));
    }

    // Do not manipulate boot-sensitive pins: GPIO0, GPIO2, GPIO15.
    // GPIO1 and GPIO3 are UART, and GPIO16 is safe but not always usable as output.
    pinMode(4, OUTPUT);
    digitalWrite(4, LOW);
    pinMode(5, OUTPUT);
    digitalWrite(5, LOW);
    pinMode(12, OUTPUT);
    digitalWrite(12, LOW);
    pinMode(13, OUTPUT);
    digitalWrite(13, LOW);
    pinMode(14, OUTPUT);
    digitalWrite(14, LOW);
    pinMode(16, OUTPUT);
    digitalWrite(16, LOW);
    pinMode(A0, INPUT);
}

void loop()
{
    serviceWebServer();

    while (Serial.available())
    {
        char ch = Serial.read();
        if (ch == '\n' && inputAfterCR) { inputAfterCR = false; continue; }
        inputAfterCR = ch == '\r';
        if (ch == '\n' || ch == '\r')
        {
            inputComment = false;
            if (inputBuffer.length() > 0 && inputBraceDepth <= 0 && !trim(inputBuffer).endsWith("|"))
            {
                handleCommand(inputBuffer);
                inputBuffer = "";
                inputBraceDepth = 0;
                inputQuoted = inputEscaped = false;
                inputQuoteChar = 0;
                inputLine = 1;
            }
            else if (inputBuffer.length() > 0) { inputBuffer += '\n'; ++inputLine; }
        }
        else
        {
            if (inputComment) {} // Braces and quotes in comments are ordinary text.
            else if (!inputQuoted && ch == '#' && (inputBuffer.length() == 0 ||
                     isspace((unsigned char)inputBuffer[inputBuffer.length()-1]) ||
                     inputBuffer.endsWith("{") || inputBuffer.endsWith(";") || inputBuffer.endsWith("|"))) inputComment = true;
            else if (inputQuoted && inputEscaped) inputEscaped = false;
            else if (inputQuoted && ch == '\\') inputEscaped = true;
            else if (inputQuoted && ch == inputQuoteChar) { inputQuoted = false; inputQuoteChar = 0; }
            else if (!inputQuoted && (ch == '"' || ch == '\'')) { inputQuoted = true; inputQuoteChar = ch; }
            else if (!inputQuoted && ch == '{') ++inputBraceDepth;
            else if (!inputQuoted && ch == '}') --inputBraceDepth;
            inputBuffer += ch;
            if (inputBuffer.length() > 4096)
            {
                Serial.print(F("ERROR: command too long at line "));
                Serial.println(inputLine);
                inputBuffer = "";
                inputBraceDepth = 0;
                inputQuoted = inputEscaped = false;
                inputQuoteChar = 0; inputComment = false;
                inputLine = 1;
            }
        }
    }

    scriptRuntime.tick((uint32_t)millis(), (uint32_t)micros());
    std::string oledError;
    if (!oledDisplay.service((uint32_t)millis(), oledError)) {
        commandOutput->print(F("ERROR: ")); commandOutput->println(oledError.c_str());
    }
    yield();
}
