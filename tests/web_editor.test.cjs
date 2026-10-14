const assert = require('node:assert/strict');
const fs = require('node:fs');
const path = require('node:path');
const vm = require('node:vm');
const sketch = fs.readFileSync(path.join(__dirname, '../firmware/esp12e_commandware/esp12e_commandware.ino'), 'utf8');
const script = sketch.match(/<script>([\s\S]*?)<\/script>/)[1];

function client(cdn = 'loaded', storage = new Map()) {
  const elements = new Map(), timers = new Map(), calls = [], assets = [];
  const confirmations = [], answers = [];
  let nextTimer = 0, mode, cm;
  function element() {
    return {value: '', selectionStart: 0, selectionEnd: 0, textContent: '', style: {}, children: [], attributes: {}, events: {},
      append(...children) {this.children.push(...children);},
      replaceChildren(...children) {this.children = children;},
      setAttribute(name, value) {this.attributes[name] = value;},
      addEventListener(name, callback) {this.events[name] = callback;},
      focus() {}, remove() {this.removed = true;},
      showModal() {this.open = true;}, close() {this.open = false;},
      setSelectionRange(start, end) {this.selectionStart = start; this.selectionEnd = end;},
      getBoundingClientRect() {return {left: 0, width: 1000};},
      setRangeText(text, start, end) {this.value = this.value.slice(0, start) + text + this.value.slice(end); this.selectionStart = this.selectionEnd = start + text.length;},
      requestSubmit() {return this.onsubmit({preventDefault() {}});}
    };
  }
  const document = {
    getElementById(id) {if (!elements.has(id)) elements.set(id, element()); return elements.get(id);},
    createElement: element, createTextNode: text => ({text}),
    head: {append(...nodes) {
      assets.push(...nodes);
      if (cdn !== 'pending') queueMicrotask(() => nodes.forEach(n => cdn === 'loaded' ? n.onload() : n.onerror()));
    }}
  };
  const context = vm.createContext({document, URLSearchParams, AbortController,
    confirm(message) {confirmations.push(message); return answers.length ? answers.shift() : true;},
    localStorage: {getItem: key => storage.has(key) ? storage.get(key) : null, setItem: (key, value) => storage.set(key, value)},
    setTimeout(callback, delay) {timers.set(++nextTimer, {callback, delay}); return nextTimer;},
    clearTimeout(id) {timers.delete(id);}, setInterval() {},
    fetch: async (_, options) => {
      const command = options.body.get('command'); calls.push(command);
      return {ok: true, text: async () => command === 'jobs' ? '' : 'OK\n' + command};
    },
    CodeMirror: {
      defineMode(name, factory) {mode = factory();},
      fromTextArea(textarea, options) {
        cm = {value: textarea.value, cursor: 0, options, getValue() {return this.value;},
          getCursor() {return {index: this.cursor};}, indexFromPos(pos) {return pos.index;},
          posFromIndex(index) {return {index};}, setCursor(pos) {this.cursor = pos.index;},
          replaceRange(text, from, to) {this.value = this.value.slice(0, from.index) + text + this.value.slice(to.index); if(this.change)this.change();},
          setValue(value) {this.value = value; if(this.change)this.change();}, focus() {}, refresh() {},
          on(event, callback) {if(event === 'change')this.change = callback;}, clearHistory() {},
          getInputField: element, toTextArea() {textarea.value = this.value;}};
        return cm;
      }
    }
  });
  vm.runInContext(script, context);
  return {context, elements, timers, calls, assets, storage, confirmations, answers, get cm() {return cm;}, get mode() {return mode;}};
}
const settle = () => new Promise(resolve => setImmediate(resolve));
const descendants = node => node.children.flatMap(child => [child, ...descendants(child)]);
const sampleButtons = node => descendants(node).filter(child => child.attributes['aria-label']?.startsWith('Insert '));

(async () => {
  const c = client();
  await settle();
  assert.equal(c.elements.get('editor-status').textContent, 'CodeMirror editor');
  const samples = c.elements.get('command-suggestions');
  assert.equal(samples.children.length, 6);
  assert(samples.children.every(group => group.open === false));
  const firstGroup = samples.children[0];
  firstGroup.children[0].onclick({preventDefault() {}});
  assert(firstGroup.open);
  const groupFilter = c.elements.get('sample-filter');
  groupFilter.value = 'OLED';groupFilter.oninput();
  assert(samples.children.every(group => group.open));
  // Collapsing a search result must not change the user's unfiltered choices.
  samples.children[0].children[0].onclick({preventDefault() {}});
  groupFilter.value = '';groupFilter.oninput();
  assert(samples.children[0].open);
  assert(samples.children.slice(1).every(group => group.open === false));
  samples.children[0].children[0].onclick({preventDefault() {}});
  assert.equal(samples.children[0].open, false);
  console.log('PASS groups start collapsed, toggle, expand for search, and restore previous choices');
  const schedule = sampleButtons(samples).find(b => b.attributes['aria-label'] === 'Insert Schedule two notes');
  schedule.onclick();
  assert.match(c.cm.value, /schedule \{\n  1000/);
  await c.elements.get('command-form').requestSubmit();
  assert.equal(c.calls.filter(command => command.startsWith('schedule')).length, 1);
  assert.equal(c.elements.get('result').textContent, 'OK\n' + c.cm.value);
  assert.equal(c.elements.get('run-command').disabled, false);
  assert.equal(sampleButtons(samples).length, 38);
  assert(!samples.children.some(b => b.textContent === 'Recent commands'));
  console.log('PASS enhanced editor submits a multiline sample once');

  c.cm.value = 'print "before"\nprint "after"';
  c.cm.cursor = 'print "before"\n'.length;
  const help = sampleButtons(samples).find(b => b.attributes['aria-label'] === 'Insert Command reference');
  help.onclick();
  assert.equal(c.cm.value, 'print "before"\nhelp\nprint "after"');
  assert.equal(c.cm.cursor, 'print "before"\nhelp\n'.length);
  c.cm.value = 'keep selected text'; c.cm.cursor = 4;
  help.onclick();
  assert.equal(c.cm.value, 'keep\nhelp\n selected text');
  console.log('PASS enhanced sample insertion preserves text on both sides of the cursor');

  const filter = c.elements.get('sample-filter');
  for (const query of ['sh1106', '200 ms', '--address 0x3d', 'OLED charts']) {
    filter.value = query; filter.oninput();
    assert(sampleButtons(samples).length, `No samples for ${query}`);
  }
  filter.value = 'no-such-command'; filter.oninput();
  assert.equal(samples.textContent, 'No matching commands.');
  filter.value = ''; filter.oninput();
  const groups = vm.runInContext('p', c.context);
  assert.equal(groups.find(group => group.group === 'OLED setup').items.length, 8);
  assert.equal(groups.flatMap(group => group.items).length, 38);
  console.log('PASS grouped samples filter titles, descriptions, commands, and categories');

  const left = c.elements.get('divider-left'), right = c.elements.get('divider-right');
  left.onkeydown({key: 'ArrowRight', preventDefault() {}});
  assert.equal(c.elements.get('workspace').style.gridTemplateColumns, '22% 53% 25%');
  right.onkeydown({key: 'ArrowLeft', preventDefault() {}});
  assert.equal(c.elements.get('workspace').style.gridTemplateColumns, '22% 51% 27%');
  console.log('PASS divider keyboard controls resize adjacent panels');

  for (const [token, expected] of [['$pitch', 'variable-2'], ['1000', 'number'], ['"hello"', 'string'], ['# comment', 'comment'], ['|', 'operator']]) {
    const stream = {eatSpace: () => false, match: regex => regex.test(token), next() {}};
    assert.equal(c.mode.token(stream), expected);
  }
  console.log('PASS Commandware highlights variables, numbers, strings, comments, and pipes');

  const offline = client('failed');
  await settle();
  assert.equal(offline.elements.get('editor-status').textContent, 'Basic editor · CDN unavailable');
  assert.equal(offline.cm, undefined);
  const input = offline.elements.get('command');
  input.value = 'do {\n  print "offline"\n}';
  await offline.elements.get('command-form').requestSubmit();
  assert.equal(offline.elements.get('result').textContent, 'OK\n' + input.value);
  assert(offline.assets.every(asset => asset.removed));
  console.log('PASS unavailable CDN retains a usable textarea and command execution');

  input.value = 'print "before"\nprint "after"';
  input.selectionStart = 0; input.selectionEnd = 'print "before"\n'.length;
  sampleButtons(offline.elements.get('command-suggestions')).find(b => b.attributes['aria-label'] === 'Insert Command reference').onclick();
  assert.equal(input.value, 'print "before"\nhelp\nprint "after"');
  assert.equal(input.selectionEnd, 'print "before"\nhelp\n'.length);
  console.log('PASS fallback insertion preserves selected text and inserts at its end');

  const upgrading = client('pending'), draft = upgrading.elements.get('command');
  draft.value = 'print "before"\nprint "after"'; draft.selectionStart = draft.selectionEnd = 15;
  upgrading.assets.forEach(asset => asset.onload());
  await settle();
  assert.equal(upgrading.cm.value, draft.value);
  assert.equal(upgrading.cm.cursor, 15);
  console.log('PASS CDN enhancement preserves the draft and cursor position');

  for (const cdn of ['loaded', 'failed']) {
    const files = client(cdn);
    await settle();
    const el = id => files.elements.get(id);
    const write = text => {
      if(files.cm)files.cm.setValue(text);
      else{el('command').value=text;el('command').events.input();}
    };
    const draftText = 'do {\n  oled print "Saved script"\n}';
    write(draftText);
    assert.match(el('script-title').textContent, /Untitled.*Unsaved/);
    el('save-script').onclick();
    assert(el('save-dialog').open);
    el('script-name').value = '  OLED demo  ';
    el('save-form').onsubmit({preventDefault() {}});
    assert.equal(el('save-dialog').open, false);
    assert.equal(el('script-title').textContent, 'OLED demo');
    assert.equal(JSON.parse(files.storage.get('commandware.scripts.v1'))[0].source, draftText);

    write(draftText + '\noled clear');
    el('save-script').onclick();
    assert.equal(JSON.parse(files.storage.get('commandware.scripts.v1')).length, 1);
    assert.equal(el('script-title').textContent, 'OLED demo');
    const savedText = files.context.editorValue();
    write('print "unsaved"');
    files.answers.push(false);el('new-script').onclick();
    assert.equal(files.context.editorValue(), 'print "unsaved"');
    files.answers.push(true);el('new-script').onclick();
    assert.equal(files.context.editorValue(), '');
    assert.equal(el('script-title').textContent, 'Untitled');

    el('open-script').onclick();
    assert.equal(el('saved-scripts').children[0].textContent, 'OLED demo');
    write('print "keep me"');
    files.answers.push(false);el('saved-scripts').children[0].onclick();
    assert.equal(files.context.editorValue(), 'print "keep me"');
    assert(el('open-dialog').open);
    files.answers.push(true);el('saved-scripts').children[0].onclick();
    assert.equal(files.context.editorValue(), savedText);
    assert.equal(el('open-dialog').open, false);
    assert.equal(el('script-title').textContent, 'OLED demo');

    const reloaded = client(cdn, files.storage);
    await settle();
    reloaded.elements.get('open-script').onclick();
    reloaded.elements.get('saved-scripts').children[0].onclick();
    assert.equal(reloaded.context.editorValue(), savedText);
    console.log(`PASS named scripts save, update, reopen after reload, and protect unsaved edits (${cdn})`);

    el('new-script').onclick();write('replacement');
    el('save-script').onclick();el('script-name').value='OLED demo';
    files.answers.push(false);el('save-form').onsubmit({preventDefault() {}});
    assert.equal(JSON.parse(files.storage.get('commandware.scripts.v1'))[0].source, savedText);
    assert(el('save-dialog').open);
    files.answers.push(true);el('save-form').onsubmit({preventDefault() {}});
    assert.equal(JSON.parse(files.storage.get('commandware.scripts.v1'))[0].source, 'replacement');
    write('quota failure draft');
    files.context.localStorage.setItem=()=>{throw Error('QuotaExceededError');};
    el('save-script').onclick();
    assert.equal(files.context.editorValue(), 'quota failure draft');
    assert.match(el('script-title').textContent, /Unsaved changes/);
    assert.match(el('file-message').textContent, /Unable to save/);
    assert.equal(JSON.parse(files.storage.get('commandware.scripts.v1'))[0].source, 'replacement');
    console.log(`PASS duplicate-name cancellation and storage failures preserve scripts and drafts (${cdn})`);
  }

  const invalid = client('failed', new Map([['commandware.scripts.v1', '{broken json']]));
  await settle();
  invalid.elements.get('command').value = 'keep my draft';
  invalid.elements.get('open-script').onclick();
  assert.match(invalid.elements.get('file-message').textContent, /Unable to read/);
  invalid.elements.get('save-script').onclick();invalid.elements.get('script-name').value='New name';
  invalid.elements.get('save-form').onsubmit({preventDefault() {}});
  assert.equal(invalid.storage.get('commandware.scripts.v1'), '{broken json');
  assert.equal(invalid.elements.get('command').value, 'keep my draft');
  assert(invalid.elements.get('save-dialog').open);
  assert.match(invalid.elements.get('save-error').textContent, /Unable to save/);
  console.log('PASS invalid stored data is reported without overwriting storage or editor text');

  const slow = client('pending');
  slow.elements.get('command').value = 'read 12';
  [...slow.timers.values()].find(timer => timer.delay === 6000).callback();
  await settle();
  slow.assets.forEach(asset => asset.onload());
  await settle();
  assert.equal(slow.cm, undefined);
  assert.equal(slow.elements.get('command').value, 'read 12');
  assert.equal(slow.elements.get('editor-status').textContent, 'Basic editor · CDN unavailable');
  console.log('PASS late CDN load preserves typed text and stays in fallback');
})().catch(error => {console.error(error); process.exitCode = 1;});
