#include "history.h"
#include <stdio.h>
#include <string.h>
static int ck=0,f=0; static void t(bool c,const char*m){ck++;if(!c){f++;printf("  FAIL: %s\n",m);}}
int main(){
    hist_reset();
    t(hist_count()==0,"empty at start");
    t(hist_get(0)==nullptr,"nothing to recall");
    hist_add("ls"); hist_add("mem"); hist_add("ver");
    t(hist_count()==3,"three recorded");
    t(strcmp(hist_get(0),"ver")==0,"depth 0 is most recent");
    t(strcmp(hist_get(1),"mem")==0,"depth 1");
    t(strcmp(hist_get(2),"ls")==0,"depth 2 is oldest");
    t(hist_get(3)==nullptr,"past the end is null");
    hist_add("ver");
    t(hist_count()==3,"a consecutive duplicate is not stored");
    hist_add("");
    t(hist_count()==3,"an empty line is not stored");
    // wraparound: fill past capacity
    hist_reset();
    char b[16];
    for(int i=0;i<HIST_N+5;i++){ snprintf(b,sizeof(b),"c%d",i); hist_add(b); }
    t(hist_count()==HIST_N,"count caps at HIST_N");
    snprintf(b,sizeof(b),"c%d",HIST_N+4);
    t(strcmp(hist_get(0),b)==0,"most recent after wraparound is the last added");
    t(strcmp(hist_get(HIST_N-1),"c5")==0,"oldest surviving entry is correct after wraparound");
    printf("\n%d/%d passed\n",ck-f,ck); return f?1:0;
}
