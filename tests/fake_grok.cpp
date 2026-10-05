#include <windows.h>
#include <atomic>
#include <fstream>
#include <iostream>
#include <mutex>
#include <string>
#include <thread>

int main() {
    char path[32768]{};
    GetEnvironmentVariableA("GROK_TEST_EVENTS", path, sizeof(path));
    std::ofstream events(path, std::ios::app);
    events << "start " << GetCurrentProcessId() << std::endl;
    std::mutex outputMutex;
    std::atomic_bool done = false;
    // Enough unsolicited output to fill a pipe if the client stops reading idle output.
    std::thread notifications([&] {
        while (!done) {
            {
                std::lock_guard lock(outputMutex);
                std::cout << "{\"method\":\"notification\",\"params\":\""
                          << std::string(5000, 'x') << "\"}" << std::endl;
            }
            Sleep(2);
        }
    });
    std::string line;
    while (std::getline(std::cin, line)) {
        const size_t at = line.find("\"id\":");
        if (at == std::string::npos) continue;
        const int id = std::stoi(line.substr(at + 5));
        std::string result = "{}";
        if (line.find("session/new") != std::string::npos) {
            events << "session" << std::endl;
            result = R"({"sessionId":"test-session"})";
        } else if (line.find("_x.ai/billing") != std::string::npos) {
            events << "billing" << std::endl;
            char mode[32]{};
            GetEnvironmentVariableA("GROK_TEST_MODE", mode, sizeof(mode));
            if (std::string(mode) == "exit") ExitProcess(1);
            if (std::string(mode) == "hang") {
                Sleep(INFINITE);
            }
            result = R"({"config":{"creditUsagePercent":25,"prepaidBalance":{"val":500}}})";
            if (std::string(mode) == "error") {
                std::lock_guard lock(outputMutex);
                std::cout << "{\"id\":" << id << ",\"error\":{\"code\":-1}}" << std::endl;
                continue;
            }
        }
        std::lock_guard lock(outputMutex);
        std::cout << "{\"id\": " << id << ",\"result\":" << result << "}" << std::endl;
    }
    done = true;
    notifications.join();
    return 0;
}
