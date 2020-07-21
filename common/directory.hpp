#pragma once
#include "platform_define.hpp"
#include "common/string.hpp"

#if defined(__GNUC__)
#include <experimental/filesystem>
namespace fs = std::experimental::filesystem;
#else
#include <filesystem>
namespace fs = std::filesystem;
#endif

namespace moon
{
    class directory
    {
        template<typename THandler>
        static void traverse_folder_imp(const fs::path& path, int depth, THandler&&handler)
        {
            if (depth < 0)
            {
                return;
            }

            if (!fs::exists(path))
            {
                return;
            }

            depth--;

            for (auto&p : fs::directory_iterator(path))
            {
                if (!handler(p.path(), fs::is_directory(p)))
                {
                    break;
                }
                if (fs::is_directory(p))
                {
                    traverse_folder_imp(p.path(), depth, std::forward<THandler>(handler));
                }
            }
        }
    public:
        inline static fs::path working_directory;

        static std::string current_directory()
        {
            std::error_code ec;
            auto p = fs::current_path(ec);
            return p.string();
        }

        static fs::path module_path()
        {
#if TARGET_PLATFORM == PLATFORM_WINDOWS
            char temp[MAX_PATH];
            auto len = GetModuleFileName(NULL, temp, MAX_PATH);
#else
            char temp[1024];
            auto len = readlink("/proc/self/exe", temp, 1024);
#endif
            std::string res(temp, len);
            return fs::path(res).parent_path();
        }

        static bool exists(const std::string &path)
        {
            std::error_code ec;
            auto b = fs::exists(path, ec);
            return (!ec) && b;
        }

        //THandler bool(const fs::path& path,bool dir)
        template<typename THandler>
        static void traverse_folder(const std::string& dir, int depth, THandler&&handler)
        {
            traverse_folder_imp(fs::absolute(dir), depth, std::forward<THandler>(handler));
        }

        static bool create_directory(const std::string& dir)
        {
            std::error_code ec;
            fs::create_directories(dir, ec);
            return !ec;
        }

        static bool remove(const std::string& dir)
        {
            std::error_code ec;
            fs::remove(dir, ec);
            return !ec;
        }

        static bool remove_all(const std::string& dir)
        {
            std::error_code ec;
            fs::remove_all(dir, ec);
            return !ec;
        }

        static std::string find(const std::string& path, const std::string& filename, int depth = 10)
        {
            std::string result;
            std::vector<std::string> searchdir = moon::split<std::string>(path, ";");
            for (const auto& v : searchdir)
            {
                traverse_folder(v, depth, [&result, &filename](const fs::path& p, bool)
                    {
                        if (p.filename().string() == filename)
                        {
                            result = fs::absolute(p).string();
                            return false;
                        }
                        return true;
                    });

                if (!result.empty())
                {
                    return result;
                }
            }
            return result;
        }
    };
}