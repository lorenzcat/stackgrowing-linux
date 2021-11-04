#include <algorithm>
#include <asm-generic/errno-base.h>
#include <cerrno>
#include <cstddef>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <fmt/core.h>
#include <ios>
#include <sys/types.h>
#include <sys/stat.h>
#include <unistd.h>
#include <iostream>
#include <fstream>
#include <string>
#include <regex>
#include <fmt/format.h>
#include <fmt/ranges.h>
#include <vector>
#include <charconv>
#include <sys/wait.h>
#include <sys/mman.h>

#include "types.h"


struct all{
    all operator*()const{return {};}
    all operator++(int){return *this;}
    bool operator==(all) const{return false;}
    bool operator!=(all) const{return true;}
    all operator++(){return *this;}
    all begin()const{return {};}
    all end()const{return {};}
};
inline constexpr all me;

int g_devnull;

class Ipc
{
public:
    int fds[4];

    Ipc()
    {
        if (pipe(this->fds) || pipe(this->fds + 2))
            perror("IPC pipe constructor failed");
    }

    int fatherRead(void *buf, u64 sz)
    {
        return read(this->fds[0], buf, sz);
    }

    int workerWrite(void *buf, u64 sz)
    {
        return write(this->fds[1], buf, sz);
    }

    int workerRead(void *buf, u64 sz)
    {
        return read(this->fds[2], buf, sz);
    }

    int fatherWrite(void *buf, u64 sz)
    {
        return write(this->fds[3], buf, sz);
    }


private:
};

namespace dbg
{
    using fmt::print;
}

struct MapEntry
{
    u64 start;
    u64 end;
    u64 size;

    MapEntry(u64 start, u64 end) :
        start(start), end(end), size(end-start)
    {

    }

    friend std::ostream & operator <<(std::ostream &out, const MapEntry &obj) {
        out << fmt::format("MapEntry(start = {:#x}, end = {:#x}, size = {:#x})", obj.start, obj.end, obj.size);
        return out;
    }

    operator std::string() const {
        std::ostringstream out;
        out << *this;
        return out.str();
    }

};

u64 fromHex(std::string &s)
{
    u64 result;
    std::from_chars(s.c_str(), s.c_str() + s.size(), result, 16);
    return result;
}

class ProcMap
{
public:

    u64 stackAddress;

    ProcMap()
    {
        stackAddress = 0;
    }

    bool add(std::string line)
    {
        std::regex rgx("^(.*?)-(.*?)\\s(.*?)\\s(.*?)\\s(.*?)\\s(.*?)\\s(.*)$");
        std::smatch match;
        std::vector<std::string> vmatch;
        if (!std::regex_match(line, match, rgx))
            return false;
        
        for (std::string m: match)
            vmatch.push_back(m);

        auto start = fromHex(vmatch[1]);
        auto end = fromHex(vmatch[2]);
        this->_entries.push_back(MapEntry(start, end));

        if ((vmatch.end() - 1)->find("stack") != std::string::npos)
            this->stackAddress = start;

        return true;
    }

    bool has(u64 addr)
    {
        for (auto e : this->_entries)
        {
            if (addr >= e.start && addr < e.end)
                return true;
        }
        return false;
    }

    void clear()
    {
        this->_entries.clear();
    }

    u64 getStackAddress()
    {
        return this->stackAddress;
        //if (this->_entries.empty())
        //    return 0;
        //return (this->_entries.end() - 4)->start;
    }

    friend std::ostream & operator <<(std::ostream &out, const ProcMap &obj) {
        //out << fmt::format("{}", obj._entries);
        out << "ProcMap:\n";
        for (auto e : obj._entries)
        {
            out << fmt::format("\t{:x} {:#x} {:#x}\n", e.start, e.end, e.size);
        }
        return out;
    }

    operator std::string() const {
        std::ostringstream out;
        out << *this;
        return out.str();
    }

private:

    std::vector<MapEntry> _entries;
    pid_t _pid;
};

void readProcMappings(ProcMap &procmap)
{
    std::string line;
    std::ifstream maps ("/proc/self/maps");
    
    if (!maps.is_open())
        return;

    while (maps.good())
    {
        std::getline(maps, line);

        procmap.add(line);
    }
}

namespace Worker
{

bool isAddressMapped(u64 addr)
{
    int wstatus = 0;

    if (write(g_devnull, (void*)addr, 1) == -1)
    {
        if (errno != EFAULT)
        {
            std::cout << "unexpected write error: " << strerror(errno) << std::endl << std::flush;
        }

        return false; 
    }

    return true;
}

void worker(Ipc &ipc)
{
    for (all t:me)
    {
        int rv;
        u64 addr;
        if (rv = ipc.workerRead(&addr, sizeof(addr)), rv != sizeof(addr))
        {
            fmt::print("workerRead: %d\n", rv);
            continue;
        }

        fmt::print("worker: checking {:#x}\n", addr);
        std::cout << std::flush;

        bool result = isAddressMapped(addr);

        if (rv = ipc.workerWrite(&result, sizeof(result)), rv != sizeof(result))
        {
            fmt::print("workerWrite: %d\n", rv);
            continue;
        }
    }
}

}


bool fork_isAddressMapped(u64 addr)
{
    int wstatus = 0;
    pid_t pid = fork();

    if (!pid)
    {
        if (write(g_devnull, (void*)addr, 1) == -1)
        {
            if (errno == EFAULT)
                exit(1);
            else
            {
                std::cout << "isAddressMapped: " << strerror(errno) << std::endl;
                exit(2);
            }
        }
        exit(0);
    }

    while (waitpid(pid, &wstatus, 0), !WIFEXITED(wstatus));
    
    return WEXITSTATUS(wstatus) == 0;
    
}


pid_t spawnWorker(Ipc &ipc)
{
    pid_t pid = fork();
    
    if (!pid)
    {
        Worker::worker(ipc);
        fmt::print("Worker {:d} exiting\n", getpid());
        exit(0);
    }
    
    return pid;
}

int main(int argc, char **argv, char **envp)
{
    ProcMap procmap;
    Ipc ipc;

    if (open("/dev/null", O_RDWR) == -1)
    {
        perror("open");
        return 1;
    }

    fmt::print("worker spawned with pid = {:d}\n", spawnWorker(ipc));
    
    procmap.clear();
    readProcMappings(procmap);
    std::cout << procmap << std::endl;
        
    u64 stack_addr = procmap.getStackAddress();
    fmt::print("stack_addr = {:#x}\n", stack_addr);
    
    u64 i;
    for( i = 0; fork_isAddressMapped(stack_addr - i*0x1000) == true; ++i);
    --i;
    fmt::print("i = {:d}, stack = {:#x}, reached = {:#x}\n", i, stack_addr, stack_addr - i*0x1000);

    for (all t:me) // undefined behaviour meme
    {
        u64 addr;
        bool response;
        std::cout << "Address: " << std::flush;
        std::cin >> std::hex >> addr;

        // ask the worker
        if (ipc.fatherWrite(&addr, sizeof(addr)) != sizeof(addr))
        {
            perror("fatherWrite");
            continue;
        }
        // get reply
        if (ipc.fatherRead(&response, sizeof(response)) != sizeof(response))
        {
            perror("fatherRead");
            continue;
        }

        std::cout << (response ? "mapped\n" : "not mapped\n") << std::flush; //<< std::endl;
    }
    
    return 0;
}
