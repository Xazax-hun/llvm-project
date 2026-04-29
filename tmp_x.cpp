#include "Inputs/lifetime-analysis.h"
int f(std::span<int> s [[clang::noescape]]){int t=0;for(int x:s)t+=x;return t;}
