#pragma once

#include "libnuraft/logger.hxx"
#include <iostream>
#include <mutex>
#include <string>

namespace Raft3D
{
    class Raft3DLogger : public nuraft::logger
    {
    private:
        int log_level_;
        std::mutex mu_;

    public:
        Raft3DLogger() : log_level_(6) {}

        void set_level(int l) override
        {
            log_level_ = l;
        }

        int get_level() override
        {
            return log_level_;
        }

        void put_details(int level,
                         const char *source_file,
                         const char *func_name,
                         size_t line_number,
                         const std::string &log_line) override
        {
            // Skip DEBUG and TRACE logs
            if (level == 5 || level == 6)
                return;
            if (level > log_level_)
                return;
            static const char *level_names[] = {
                "INVALID", "FATAL", "ERROR", "WARN", "INFO", "DEBUG", "TRACE"};
            static const char *level_colors[] = {
                "\033[0m"     // 0 - INVALID - reset (no color)
                "\033[1;31m", // 1 - FATAL - bright red
                "\033[31m",   // 2 - ERROR - red
                "\033[33m",   // 3 - WARN  - yellow
                "\033[32m",   // 4 - INFO  - green
                "\033[36m",   // 5 - DEBUG - cyan
                "\033[0m"     // 6 - TRACE - reset (no color)
            };
            int idx = level;
            if (idx < 0 || idx > 5)
                idx = 5;
            std::lock_guard<std::mutex> lock(mu_);
            std::cout << level_colors[idx]
                      << "[" << level_names[idx] << "] "
                      << source_file << ":" << line_number << " (" << func_name << "): "
                      << log_line << "\033[0m" << std::endl;
        }

        // Optionally override these for backward compatibility
        void debug(const std::string &log_line) override
        {
            // put_details(5, "unknown", "unknown", 0, log_line);
        }
        void info(const std::string &log_line) override
        {
            put_details(4, "unknown", "unknown", 0, log_line);
        }
        void warn(const std::string &log_line) override
        {
            put_details(3, "unknown", "unknown", 0, log_line);
        }
        void err(const std::string &log_line) override
        {
            put_details(2, "unknown", "unknown", 0, log_line);
        }
    };
} // namespace Raft3D
