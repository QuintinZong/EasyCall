// EasyCall 自检工具(开发用): xlsxtest.exe <文件> [--net]
// 解析 xlsx/csv 并把结果写入 xlsxtest.out; --net 做 TCP 帧回环测试
#include "ec_common.h"
#include "ec_xlsx.h"
#include <cstdio>
#include <string>
#include <thread>

static void NetLoopback() {
    EcNetStart();
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(26800);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(srv, (sockaddr*)&sa, sizeof sa);
    listen(srv, 1);
    std::thread t([&]() {
        sockaddr_in from; int fl = sizeof from;
        SOCKET c = accept(srv, (sockaddr*)&from, &fl);
        bool ok = false;
        std::string f = TcpRecvFrame(c, 5000, &ok);
        FILE* fh = fopen("xlsxtest.out", "ab");
        if (fh) {
            if (ok && f == "HELLO 你好 123") fprintf(fh, "net-loopback: PASS\n");
            else fprintf(fh, "net-loopback: FAIL got=%s ok=%d\n", f.c_str(), ok);
            fclose(fh);
        }
        closesocket(c);
    });
    Sleep(200);
    SOCKET cli = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    connect(cli, (sockaddr*)&sa, sizeof sa);
    TcpSendFrame(cli, "HELLO 你好 123");
    t.join();
    closesocket(cli);
    closesocket(srv);
    EcNetStop();
}

static void EchoServer() {
    // 监听 26801, 循环: 收帧 -> 回显 "PONG", 连接保持
    EcNetStart();
    SOCKET srv = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(26801);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    bind(srv, (sockaddr*)&sa, sizeof sa);
    listen(srv, 1);
    for (;;) {
        sockaddr_in from; int fl = sizeof from;
        SOCKET c = accept(srv, (sockaddr*)&from, &fl);
        if (c == INVALID_SOCKET) continue;
        for (;;) {
            bool ok = false;
            std::string f = TcpRecvFrame(c, 30000, &ok);
            if (!ok) break;
            TcpSendFrame(c, "PONG");
        }
        closesocket(c);
    }
}

static void EchoClient() {
    // 连接 127.0.0.1:26801, 发 5 个 PING, 每个都读回 PONG
    EcNetStart();
    FILE* fh = fopen("xlsxtest.out", "wb");
    SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons(26801);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int okCount = 0;
    if (connect(c, (sockaddr*)&sa, sizeof sa) != 0) {
        fprintf(fh, "client: connect failed\n");
    } else {
        for (int i = 0; i < 5; i++) {
            if (!TcpSendFrame(c, "PING")) { fprintf(fh, "client: send fail at %d\n", i); break; }
            bool ok = false;
            std::string f = TcpRecvFrame(c, 3000, &ok);
            if (ok && f == "PONG") okCount++;
            else { fprintf(fh, "client: recv fail at %d ok=%d\n", i, ok); break; }
            Sleep(100);
        }
    }
    fprintf(fh, "client roundtrips: %d / 5\n", okCount);
    fclose(fh);
    closesocket(c);
    EcNetStop();
}

static void PushFrame(int port, const std::string& text) {
    // 一次性发送一个帧(端到端测试): --push <port> <text>
    // 命令行参数可能是 ANSI/GBK, 统一转成协议要求的 UTF-8
    std::string payload;
    if (MultiByteToWideChar(CP_UTF8, MB_ERR_INVALID_CHARS, text.data(), (int)text.size(), nullptr, 0) > 0) {
        payload = text;   // 已是合法 UTF-8
    } else {
        int n = MultiByteToWideChar(936, 0, text.data(), (int)text.size(), nullptr, 0);
        std::wstring w(n, 0);
        MultiByteToWideChar(936, 0, text.data(), (int)text.size(), &w[0], n);
        payload = WU8(w);
    }
    EcNetStart();
    SOCKET c = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    sockaddr_in sa;
    memset(&sa, 0, sizeof sa);
    sa.sin_family = AF_INET;
    sa.sin_port = htons((u_short)port);
    sa.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    int ok = 0;
    if (connect(c, (sockaddr*)&sa, sizeof sa) == 0) {
        ok = TcpSendFrame(c, payload) ? 1 : 0;
        Sleep(150);
    }
    closesocket(c);
    EcNetStop();
    FILE* fh = fopen("xlsxtest.out", "wb");
    if (fh) { fprintf(fh, "push=%d len=%d\n", ok, (int)payload.size()); fclose(fh); }
}

int main(int argc, char** argv) {
    if (argc >= 2 && std::string(argv[1]) == "--net") { NetLoopback(); return 0; }
    if (argc >= 2 && std::string(argv[1]) == "--server") { EchoServer(); return 0; }
    if (argc >= 2 && std::string(argv[1]) == "--client") { EchoClient(); return 0; }
    if (argc >= 4 && std::string(argv[1]) == "--push") {
        PushFrame(atoi(argv[2]), std::string(argv[3]));
        return 0;
    }
    if (argc < 2) {
        FILE* fh = fopen("xlsxtest.out", "wb");
        fprintf(fh, "usage: xlsxtest <file.xlsx|csv> [--net]\n");
        fclose(fh);
        return 1;
    }
    std::vector<XRow> rows;
    std::wstring err;
    bool ok = ImportSpreadsheet(U8W(argv[1]), rows, err);
    FILE* fh = fopen("xlsxtest.out", "wb");
    if (!ok) {
        fprintf(fh, "import-fail: %s\n", WU8(err).c_str());
        fclose(fh);
        return 1;
    }
    fprintf(fh, "rows: %d\n", (int)rows.size());
    for (auto& r : rows)
        fprintf(fh, "row: [%s] | [%s] | [%s]\n", WU8(r.id).c_str(), WU8(r.name).c_str(), WU8(r.cls).c_str());
    fclose(fh);
    return 0;
}
