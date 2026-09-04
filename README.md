# EasyCall 班级叫号系统

教师端一键叫号，班级大屏实时显示。
适配**Windows系统**。

技术栈：C++

## 文件说明

| 文件 | 说明 |
|---|---|
| `dist/EasyCall-Teacher.exe` | 教师端：导入名单、勾选叫号、发送清屏 |
| `dist/EasyCall-Board.exe`   | 班级大屏端：全屏大字显示被叫学生 |
| `build.bat`                 | 一键重新构建(需要 MinGW-w64 的 g++) |

## 网络模式选择

**模式一：局域网直连(同一网络/教室WiFi，无需服务器)**
1. 教室电脑运行 `EasyCall-Board.exe`(默认自动监听 25800 端口并广播自己)。
2. 教师电脑运行 `EasyCall-Teacher.exe`，选择「局域网直连」，点击【扫描教室】自动找到大屏；
   找不到时可手动输入大屏 IP(大屏顶部状态栏会显示本机 IP)。
3. 勾选学生 → 点击【叫号】。

~~模式二：服务器中转(教师与教室在不同网络，连接公网服务器作为跳板)~~

***(怕服务器被打，暂不公开服务器端代码，但提供该选项，如有该方面需求可自行编写或求助AI)***

## 使用流程(教师端)

1. 点【导入Excel】选择学生名单(支持 `.xlsx` 与 `.csv`)。
   表格前两列为 **学号、姓名**，第三列班级/备注可选，自动跳过表头行。
   旧版 `.xls` 请先在 Excel 中另存为 `.xlsx` 或 `.csv`。
2. 勾选一名或多名学生(支持全选/全不选/反选)，点击【叫号】。
4. 点击【发送清屏】可清空大屏当前显示。

## Excel 格式示例

| 学号 | 姓名 | 班级 |
|---|---|---|
| 114514 | 张三 | 54188班 |
| 1919810 | 李四 | 54188班 |

## 常见问题

- **扫描不到教室大屏**：确认两端连的是同一个路由器/WiFi；部分网络禁止广播，
  可手动输入大屏 IP；检查 Windows 防火墙是否放行 25800/25801 端口(首次运行弹窗点允许)。
- **端口被占用**：在大屏端设置里切换一个端口(教师端地址里填写 `IP:端口`)。

## 重新构建

本地双击 `build.bat`（构建完成窗口驻留，按任意键关闭）；CI/自动调用用 `build.bat -nopause`。
需要 `g++`(MinGW-w64) 与 `windres`，产物输出至 `dist/`。

## 自动构建 (GitHub Actions)

`.github/workflows/build.yml` 推送后自动运行：

1. **触发**：推送到 `main`/`master`、PR、`v*` 标签、或 Actions 页面手动运行；
2. **构建**：CI 安装 MSYS2 + MinGW-w64（同款工具链）→ `build.bat -nopause` 编译两个 exe；
3. **冒烟**：用 `--ui=` 自截图验证界面渲染（尽力而为，失败不阻断）；
4. **打包**：`EasyCall-Windows-x64.zip`（仅含两个 exe）；
5. **交付**：
   - 每次构建 zip 上传为 Artifact（Actions 运行页下载，保留 30 天）；
   - **推 `main`/`master`**：自动发布 **Pre-Release**（滚动更新 `dev-build` 标签，仓库主页可见最新预发布产物）；
   - **推 `v*` 标签**：发布**正式 Release**：
     ```bash
     git tag v1.0 && git push origin v1.0
     ```
   - **手动运行**（Actions → 本工作流 → Run workflow）可选择发布模式：
     `none`(仅构建) / `prerelease`(Pre-Release) / `release`(正式版，可填版本号，留空自动为 `manual-v构建号`)。

配套 `.gitignore`（忽略构建产物/运行数据）与 `.gitattributes`（bat 强制 CRLF，避免 CI 上批处理解析出错）已就绪，`git add .` 即可。