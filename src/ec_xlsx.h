// EasyCall - Excel / CSV 导入
#pragma once
#include <string>
#include <vector>

struct XRow {
    std::wstring id;    // 学号(第一列)
    std::wstring name;  // 姓名(第二列)
    std::wstring cls;   // 班级/备注(第三列, 可空)
};

// 支持 .xlsx 与 .csv(UTF-8 或 ANSI/GBK); 读取第一个工作表的前三列
bool ImportSpreadsheet(const std::wstring& path, std::vector<XRow>& out, std::wstring& err);
