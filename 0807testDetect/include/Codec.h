#pragma once

#include "CodecLib_global.h"
#include <string>
#include <vector>
#include <cstdint>

class CODECLIB_EXPORT Codec
{
public:
    Codec();
    ~Codec();

    // 生成下一个15位组合编码 
    static std::string nextCode(const std::string& bits);

    // 原始ID转15位二进制编码字符串 
    static std::string rawIdToCode(long long rawId);

    // 15位编码字符串转ID 
    static long long codeToId(const std::string& bits);

    // 调整原始ID（处理ID间隙） 
    static long long adjustId(long long rawId);

    // 编码旋转归一化（处理旋转） 
    static std::vector<uint16_t> adjustCode(uint16_t code);
};
