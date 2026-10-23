// Portable execution-contract checks independent of Arduino transport/hardware.
#include "ScriptRuntime.h"
#include <cassert>
#include <iostream>
#include <vector>
int main(){
    cw::Runtime r; unsigned long id=0; std::vector<std::string> printed;
    r.newId=[&](){return ++id;};r.output=[](const std::string&){};r.release=[](unsigned long){};
    r.host=[&](const std::string&n,const std::vector<cw::Value>&args,cw::Event&e,unsigned long,std::string&error){
        if(n=="print"){if(args.empty()&&!e.hasValue){error="missing value";return false;}printed.push_back(args.empty()?e.value.str():args[0].str());return true;}
        if(n=="adc"){e.value=cw::Value(512);e.hasValue=true;return true;}
        return false;
    };
    auto run=[&](const std::string&s,uint32_t ms=0){assert(r.submit(s,ms,ms*1000));for(int i=0;i<20;++i)r.tick(ms,ms*1000);assert(r.failures.empty());};
    run("func Pick x { set count $x; max 5 $x }\nPick --x 8 | print");
    assert(printed.back()=="8");assert(r.globals->vars["count"].number==8);
    run("max 5 10 | do { max 3 4; print $value } | print");
    assert(printed[printed.size()-2]=="10"&&printed.back()=="10");
    run("max 1 2 | do { where 0 | print 99; print $value } | print");
    assert(printed[printed.size()-2]=="2"&&printed.back()=="2");
    run("split ':' 'a:b' | each --out part { print $part } | len $value | print");
    assert(printed[printed.size()-3]=="a"&&printed[printed.size()-2]=="b"&&printed.back()=="2");
    run("max 5 10 | if 0 { print 99 } else { print $value } | print");assert(printed.back()=="10");
    run("print 'Literal \\$missing'");assert(printed.back()=="Literal $missing");
    assert(!r.submit("do",0,0));assert(!r.submit("func f {}\nfunc f {}",0,0));assert(!r.submit("call Pick 1",0,0));
    run("func Later delay { schedule { $delay { print $count } } }\nLater 10\nset count 13");
    r.tick(10,10000);assert(printed.back()=="13");assert(r.tasks.empty());
    run("every 10 --out tick | toggle | print",20);r.tick(20,20000);assert(printed.back()=="1");r.tick(30,30000);assert(printed.back()=="0");r.reset();
    size_t start=printed.size();run("every 10 | pulse 25 | print",100);r.tick(100,100000);assert(printed.size()==start+1&&printed.back()=="1");r.tick(110,110000);assert(printed.size()==start+1);r.tick(120,120000);assert(printed.size()==start+1);r.tick(130,130000);assert(printed.size()==start+1);r.reset();
    run("max 0 1 | pulse 10 | print",200);r.tick(210,210000);assert(printed.back()=="0"&&r.tasks.empty());
    start=printed.size();run("do { pulse 10 } | print",300);r.tick(310,310000);assert(printed.size()==start+2&&printed[start]=="1"&&printed.back()=="0");
    start=printed.size();run("func Pulsing { pulse 10 }; Pulsing | print",400);r.tick(410,410000);assert(printed.size()==start+2&&printed.back()=="0");
    start=printed.size();run("do { do { pulse 10 } } | print",500);r.tick(510,510000);assert(printed.size()==start+2&&printed.back()=="0");
    start=printed.size();run("do { pulse 10; max 2 3 } | print",600);r.tick(610,610000);assert(printed.size()==start+1&&printed.back()=="3");
    start=printed.size();run("do { pulse 10 --out level } | print $level",700);r.tick(710,710000);assert(printed.size()==start+2&&printed.back()=="0");
    r.reset();r.functions.clear();
    for(int i=0;i<8;++i){std::string definition="func F"+std::to_string(i)+" {";for(int n=0;n<14;++n)definition+="print 0;";definition+="}";assert(r.submit(definition,0,0));}
    std::string overflow="func Overflow {";for(int n=0;n<14;++n)overflow+="print 0;";overflow+="}";assert(!r.submit(overflow,0,0));assert(r.error.find("retained command limit")!=std::string::npos);assert(!r.functions.count("Overflow"));
    r.reset();r.functions.clear();
    for(int i=0;i<7;++i)run("set g"+std::to_string(i)+" '"+std::string(1000,'x')+"'");
    assert(r.submit("set g7 '"+std::string(1000,'y')+"'",0,0));assert(!r.failures.empty());assert(!r.globals->vars.count("g7"));r.reset();
    assert(r.submit("tone 1 --hz 440 --dur 0",0,0));assert(!r.failures.empty());r.reset();
    run("tone '12' --hz '440' --dur '0'");
    start=printed.size();run("clock 100 --dur 35 | tone 12 --hz 1000 --dur 2 | print",800);for(uint32_t ms=801;ms<=840;++ms)r.tick(ms,ms*1000);assert(printed.size()==start+6);for(size_t i=0;i<6;++i)assert(printed[start+i]==(i%2?"0":"1"));assert(r.tasks.empty());
    start=printed.size();run("clock 10 --dur 200 | print",1000);r.tick(1125,1125000);assert(printed.size()==start);r.tick(1150,1150000);assert(printed.size()==start+1&&printed.back()=="1");r.reset();
    start=printed.size();run("func LabelPulse delay label { pulse $delay | print \"$label:$value\" }; clock 500 --dur 3 | LabelPulse (5 + $value * 5) (string $value)",1300);for(uint32_t ms=1301;ms<=1320;++ms)r.tick(ms,ms*1000);assert(printed.size()==start+4&&printed[start+2]=="0:0"&&printed[start+3]=="1:0");
    start=printed.size();run("do { pulse 10 | print; print 'finished' }",1400);assert(printed.size()==start+1&&printed.back()=="1");r.tick(1410,1410000);assert(printed.size()==start+3&&printed[start+1]=="0"&&printed.back()=="finished");
    start=printed.size();run("split ':' 'A:B' | each { pulse 10 | print }",1500);assert(printed.size()==start+1);r.tick(1510,1510000);assert(printed.size()==start+3&&printed[start+1]=="0"&&printed[start+2]=="1");r.tick(1520,1520000);assert(printed.size()==start+4&&printed.back()=="0"&&r.tasks.empty());

    std::cout<<"Runtime contract checks passed\n";
}
