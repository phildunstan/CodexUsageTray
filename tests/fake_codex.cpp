#include <windows.h>
#include <fstream>
#include <iostream>
#include <string>

int main() {
    char path[32768]{};
    GetEnvironmentVariableA("CODEX_TEST_EVENTS", path, sizeof(path));
    std::ofstream events(path, std::ios::app);
    events << "start " << GetCurrentProcessId() << std::endl;
    char mode[32]{};
    GetEnvironmentVariableA("CODEX_TEST_MODE", mode, sizeof(mode));
    for (std::string line; std::getline(std::cin, line);) {
        if (line.find("\"method\":\"initialized\"") != std::string::npos) {
            events << "initialized" << std::endl;
            continue;
        }
        const auto at = line.find("\"id\":");
        if (at == std::string::npos) continue;
        const int id = std::stoi(line.substr(at + 5));
        if (line.find("account/read") != std::string::npos) {
            events << (line.find("\"refreshToken\":true") != std::string::npos ? "refresh" : "no-refresh") << std::endl;
            if (std::string(mode) == "exit") return 1;
            if (std::string(mode) == "error") {
                std::cout << "{\"id\": " << id << ",\"error\":{\"code\":-1}}" << std::endl;
            } else {
                std::cout << "{\"id\": " << id << ",\"result\":{\"account\":"
                    << (std::string(mode) == "missing" ? "null" : "{\"type\":\"chatgpt\"}")
                    << "}}" << std::endl;
            }
        } else if (line.find("initialize") != std::string::npos) {
            std::cout << "{\"method\":\"notification\",\"params\":{}}" << std::endl;
            std::cout << "{\"id\": " << id << ",\"result\":{}}" << std::endl;
        } else {
            events << "unexpected-request" << std::endl;
            std::cout << "{\"id\": " << id << ",\"error\":{\"code\":-1}}" << std::endl;
        }
    }
    return 0;
}
