#include "ScriptStorage.h"
#include <cassert>
#include <iostream>
using cw::Value;
static Value text(const std::string &s) { return Value(s); }
int main() {
    cw::ScriptStorage store;
    std::string error;
    Value value;
#ifdef ESP8266
    LittleFS.configFails=true;
    assert(!store.begin());
    assert(std::string(store.mountError()).find("configuration failed")!=std::string::npos);
    LittleFS.configFails=false;
#endif
    LittleFS.directory=false;LittleFS.mkdirFails=true;
    assert(!store.begin());
    assert(std::string(store.mountError()).find("creation failed")!=std::string::npos);
    LittleFS.mkdirFails=false;LittleFS.directory=true;LittleFS.openFails=true;
    assert(!store.begin());
    assert(std::string(store.mountError()).find("could not be opened")!=std::string::npos);
    LittleFS.openFails=false;LittleFS.directory=false;LittleFS.files["/scripts"]="file";
    assert(!store.begin());
    assert(std::string(store.mountError()).find("not a directory")!=std::string::npos);
    LittleFS.files.clear();LittleFS.directory=true;
    LittleFS.mountFails=true;
    assert(!store.begin() && !LittleFS.autoFormat);
    assert(std::string(store.mountError()).find("mount failed")!=std::string::npos);
    assert(LittleFS.formatCalls==0);
    assert(!store.load("demo",value,error));
    LittleFS.mountFails=false;
    assert(store.begin() && !store.mountError());
    assert(store.list(value,error) && value.list.empty());
    assert(!store.load("missing",value,error));
    const std::string source="  print \"$literal \\\"é雪\"\n\n";
    assert(store.save("OLED demo",text(source),error));
    assert(store.load("OLED demo",value,error) && value.text==source);
    cw::ScriptStorage reboot;
    assert(reboot.begin() && reboot.load("OLED demo",value,error) && value.text==source);
    assert(store.save("empty",text(""),error));
    assert(store.load("empty",value,error) && value.text.empty());
    assert(store.save("large",text(std::string(4096,'a')),error));
    assert(store.load("large",value,error) && value.text.size()==4096);
    assert(!store.save("large",text(std::string(4097,'a')),error));
    assert(!store.save("bad",text(std::string("\xff")),error));
    for(auto name:{"", "../bad", "a/b", "a.b", " leading", "trailing ", "é", "abcdefghijklmnopqrstuvwxyz123"}) {
        cw::Event event;event.hasValue=true;event.value=text("draft");
        assert(!store.command({text("save"),text(name)},event,error));
    }
    for(auto failure:{&LittleFS.writeFails,&LittleFS.readFails,&LittleFS.flushFails,&LittleFS.renameFails,&LittleFS.openFails}) {
        *failure=true;
        assert(!store.save("OLED demo",text("replacement"),error));
        *failure=false;
        assert(store.load("OLED demo",value,error) && value.text==source);
    }
    LittleFS.files["/scripts/.pending"]="interrupted";
    LittleFS.removeFails=true;
    assert(!store.save("OLED demo",text("replacement"),error));
    LittleFS.removeFails=false;
    assert(store.save("OLED demo",text(source),error));
    LittleFS.capacity=LittleFS.usedBytes();
    assert(!store.save("OLED demo",text("replacement"),error));
    LittleFS.capacity=256*1024;
    LittleFS.removeFails=true;
    assert(!store.remove("OLED demo",error));
    LittleFS.removeFails=false;
    assert(store.remove("OLED demo",error) && !store.remove("OLED demo",error));
    LittleFS.files.clear();
    for(int i=0;i<32;++i)assert(store.save("s"+std::to_string(i),text("x"),error));
    assert(!store.save("overflow",text("x"),error));
    assert(store.save("s0",text("updated"),error));
    assert(store.list(value,error) && value.list.size()==32);
    cw::Runtime runtime;unsigned long id=0;std::vector<std::string> printed;
    runtime.newId=[&](){return ++id;};
    runtime.host=[&](const std::string &command,const std::vector<Value> &args,cw::Event &event,unsigned long,std::string &err){
        if(command=="script")return store.command(args,event,err);
        if(command=="print"){printed.push_back(cw::formatValue(args.empty()?event.value:args[0]));return true;}
        return false;
    };
    LittleFS.files.clear();
    assert(runtime.submit("string 'print 123' | script save demo | print",0,0));
    assert(runtime.failures.empty() && printed.back()=="print 123");
    assert(runtime.submit("script load demo --out source | script save copy | print $source",0,0));
    assert(runtime.failures.empty() && printed.back()=="print 123");
    assert(runtime.submit("script list --out names | len $names | print",0,0));
    assert(runtime.failures.empty() && printed.back()=="2");
    assert(!runtime.submit("script",0,0));
    assert(!runtime.submit("script save a --out x",0,0));
    assert(runtime.submit("script save a",0,0) && !runtime.failures.empty());
    runtime.reset();
    assert(runtime.submit("number 5 | script save a",0,0) && !runtime.failures.empty());
    runtime.reset();
    auto input=std::make_shared<cw::Event>(), result=std::make_shared<cw::Event>();
    input->hasValue=true;input->value=text(std::string(4096,'a'));
    assert(runtime.submit("script save full",0,0,input,result) && runtime.failures.empty());
    assert(runtime.submit("script load full",0,0,{},result) && runtime.failures.empty());
    assert(result->hasValue && result->value.text==input->value.text);
    std::cout<<"Script storage checks passed\n";
}
