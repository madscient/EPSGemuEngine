#pragma once
// emu.h
// mpeg_audio.cpp を無改変で使うためのシム。
// MAME では巨大なフレームワークヘッダだが、mpeg_audio.cpp が実際に必要と
// するのは固定幅整数と数学関数・メモリ操作・assert だけなので、
// それらを供給するだけで足りる。

#include <cassert>
#include <cmath>
#include <cstdint>
#include <cstring>
