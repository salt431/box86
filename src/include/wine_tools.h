#ifndef __WINE_TOOLS_H__
#define __WINE_TOOLS_H__

void wine_prereserve(const char* reserve);

int wine_preloaded;
void* get_wine_prereserve();

#endif //__WINE_TOOLS_H__