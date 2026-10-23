#pragma once
// In-memory Arduino FS double with explicit failure injection; no host files.
#include <algorithm>
#include <cstdint>
#include <map>
#include <memory>
#include <string>
#include <vector>
#ifndef ESP32
#define ESP8266 1
#endif
struct LittleFSConfig { bool autoFormat = true; void setAutoFormat(bool value) { autoFormat = value; } };
struct FSInfo { size_t totalBytes, usedBytes; };
struct FakeFS;
struct FakeFile {
    FakeFS *fs = nullptr;
    std::string filename;
    size_t pos = 0, index = 0;
    std::vector<std::string> entries;
    bool valid = false, directory = false;
    operator bool() const { return valid; }
    bool isDirectory() const { return directory; }
    const char *name() const { return filename.c_str(); }
    size_t size() const;
    size_t read(uint8_t *, size_t);
    size_t write(const uint8_t *, size_t);
    void flush();
    void close() { valid = false; }
    FakeFile openNextFile();
};
struct FakeDir {
    FakeFS *fs;
    std::vector<std::string> entries;
    size_t index = 0;
    bool next() { return index < entries.size() ? (++index, true) : false; }
    bool isFile() const { return true; }
    std::string fileName() const { return entries[index-1]; }
};
struct FakeFS {
    std::map<std::string, std::string> files;
    bool configFails=false, formatFails=false, mountFails=false, openFails=false, writeFails=false, readFails=false, renameFails=false,
         removeFails=false, infoFails=false, flushFails=false, mkdirFails=false, directory=true;
    bool autoFormat=true;
    size_t formatCalls=0;
    size_t capacity=256*1024;
    bool setConfig(const LittleFSConfig &config) { autoFormat=config.autoFormat; return !configFails; }
    bool begin(bool formatOnFail=false) {
#ifdef ESP32
        autoFormat=formatOnFail;
#else
        (void)formatOnFail;
#endif
        return !mountFails;
    }
    void end() {}
    bool format() {
        ++formatCalls;
        if (formatFails) return false;
        files.clear(); directory=false; return true;
    }
    bool exists(const char *path) { return (std::string(path)=="/scripts" && directory) || files.count(path); }
    bool mkdir(const char *) { if(mkdirFails)return false; directory=true; return true; }
    bool remove(const char *path) { return !removeFails && files.erase(path); }
    bool rename(const char *from, const char *to) {
        if(renameFails||!files.count(from))return false;
        files[to]=files[from];files.erase(from);return true;
    }
    size_t totalBytes() const { return capacity; }
    size_t usedBytes() const { size_t size=0;for(auto &file:files)size+=file.second.size();return size; }
    bool info(FSInfo &info) { info={totalBytes(),usedBytes()};return !infoFails; }
    std::vector<std::string> entries() {
        std::vector<std::string> result;
        for(auto &f:files)if(f.first.find("/scripts/")==0)result.push_back(f.first);
        return result;
    }
    FakeFile open(const char *path, const char *mode) {
        FakeFile file;file.fs=this;file.filename=path;
        if(openFails)return file;
        if(file.filename=="/scripts"&&directory){file.valid=true;file.directory=true;file.entries=entries();return file;}
        if(mode[0]=='w')files[path]="";
        file.valid=files.count(path);return file;
    }
    FakeDir openDir(const char *) { return {this, entries(), 0}; }
} static LittleFS;
inline size_t FakeFile::size() const { return fs->files[filename].size(); }
inline size_t FakeFile::read(uint8_t *buffer,size_t count) {
    if(fs->readFails)return 0;
    const auto &text=fs->files[filename];count=std::min(count,text.size()-pos);
    std::copy(text.begin()+pos,text.begin()+pos+count,buffer);pos+=count;return count;
}
inline size_t FakeFile::write(const uint8_t *buffer,size_t count) {
    if(fs->writeFails)count/=2;
    fs->files[filename].append(reinterpret_cast<const char*>(buffer),count);return count;
}
inline void FakeFile::flush() { if(fs->flushFails)fs->files[filename]+='!'; }
inline FakeFile FakeFile::openNextFile() {
    if(index>=entries.size())return {};
    return fs->open(entries[index++].c_str(),"r");
}
