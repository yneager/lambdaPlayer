#include <windows.h>
#include <fstream>
#include <filesystem>
int WINAPI wWinMain(HINSTANCE, HINSTANCE, PWSTR, int) {
    wchar_t module[32768], health[32768];
    GetModuleFileNameW(nullptr,module,32768);
    const auto folder=std::filesystem::path(module).parent_path();
    std::ifstream version(folder/L"version.txt"); std::string value; version>>value;
    if(value=="bad") return 1;
    if(GetEnvironmentVariableW(L"LAMBDA_UPDATE_HEALTH_FILE",health,32768)) {
        std::ofstream file{std::filesystem::path(health)}; file<<(value=="wrong" ? "v0.2.6" : "v0.2.8");
    }
    Sleep(1000);
    return 0;
}
