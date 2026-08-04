// Host check of the OS core logic that has no hardware in it: the command
// registry, and that the package-style app relocates against the 1.1 ABI.
#include "command.h"
#include "loader.h"
#include "elf.h"
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <sys/mman.h>

// registry lives in command.cpp; loader in loader.cpp
static int checks=0, fails=0;
static void ck(bool c, const char* m){checks++; if(!c){fails++; printf("  FAIL: %s\n",m);}}
static int dummy(int,char**){return 0;}

// fake SRAM + ABI for the loader half
static const uintptr_t kBase=0x30000000u; static uint8_t* g_sram; static size_t g_used;
struct Blk{uint32_t size,free;};
extern "C" {
static void* a(size_t n){n=(n+7)&~7ull; for(size_t o=0;o<g_used;){Blk*b=(Blk*)(g_sram+o); if(b->free&&b->size>=n){b->free=0;return g_sram+o+sizeof(Blk);} o+=sizeof(Blk)+b->size;} Blk*b=(Blk*)(g_sram+g_used); b->size=n;b->free=0; void*p=g_sram+g_used+sizeof(Blk); g_used+=sizeof(Blk)+n; return p;}
static void f(void*p){if(!p)return; ((Blk*)((uint8_t*)p-sizeof(Blk)))->free=1;}
}
static const char* names[]={"fw_printf","fw_millis","fw_malloc","fw_free","fw_log","rpc_register_command"};
uint32_t api_lookup(const char* n){for(auto s:names) if(!strcmp(s,n)) return 0x10000100u; return 0;}
uint32_t api_symbol_count(){return 6;}
int api_index_of(const char* n){int i=0;for(auto s:names){if(!strcmp(s,n))return i;i++;}return -1;}
uint32_t api_addr_at(uint32_t i){return i<6?0x10000100u:0;}

struct FC{FILE*f;};
static int fread_(void*c,uint32_t o,void*d,uint32_t l){FC*x=(FC*)c; if(fseek(x->f,o,SEEK_SET))return -1; return (int)fread(d,1,l,x->f);}

int main(int argc,char**argv){
    // --- registry ---
    Command c1{"alpha","a",dummy,nullptr};
    Command c2{"beta","b",dummy,(void*)0x1234};   // app-owned
    Command c3{"alpha","dup",dummy,nullptr};
    ck(cmd_register(&c1),"register a command");
    ck(cmd_register(&c2),"register an app-owned command");
    ck(!cmd_register(&c3),"a duplicate name is refused, not shadowed");
    ck(cmd_find("beta")!=nullptr,"find an app command");
    ck(cmd_count()==2,"two commands registered");
    cmd_remove_owner((void*)0x1234);
    ck(cmd_count()==1,"removing an owner sweeps only its commands");
    ck(cmd_find("beta")==nullptr,"the app command is gone");
    ck(cmd_find("alpha")!=nullptr,"the built-in survives");
    cmd_remove_owner(nullptr);
    ck(cmd_count()==1,"remove_owner(nullptr) never touches built-ins");

    // --- greet.app loads against the 1.1 ABI ---
    if(argc>1){
        g_sram=(uint8_t*)mmap((void*)kBase,1<<20,PROT_READ|PROT_WRITE|PROT_EXEC,MAP_PRIVATE|MAP_ANONYMOUS|MAP_FIXED_NOREPLACE,-1,0);
        loader_set_allocator(a,f);
        FILE* fp=fopen(argv[1],"rb"); fseek(fp,0,SEEK_END); uint32_t sz=ftell(fp);
        FC ctx{fp}; AppSource src{&ctx,fread_,sz};
        LoadedApp app; LoadResult rc=app_load(src,&app); fclose(fp);
        ck(rc==LOAD_OK,"the greet package loads and relocates");
        if(rc==LOAD_OK){
            ck(app.entry!=nullptr,"its app_main resolved");
            ck(app.veneers_used>0,"and it needs veneers for the ABI calls");
            printf("  greet: image=%uB veneers=%uB\n",app.image_size,app.veneers_used);
        } else printf("  load rc=%s %s\n",load_result_str(rc),app.detail);
    }
    printf("\n%d/%d passed\n",checks-fails,checks);
    return fails?1:0;
}
