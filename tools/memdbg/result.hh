#ifndef KRX_MEMDBG_RESULT_HH
#define KRX_MEMDBG_RESULT_HH

#include <string>
#include <map>
#include <vector>

struct Allocation {
    int pid;
    std::string zone;
    std::vector<std::string> *addresses;
};

extern std::map<std::string, Allocation> allocs;

#endif /* KRX_MEMDBG_RESULT_HH */
