#include "utils.hpp"

#include <cstring>
#include <fstream>
#include <iomanip>
#include <string>
#include <vector>

#include <openssl/sha.h>
#include <dlfcn.h>

std::vector<std::string> Utils::strsplit(char *str, const char *delimeter)
{
	auto splits = std::vector<std::string>();

	char* split = strtok(str, delimeter);
	splits.emplace(splits.end(), std::string(split));

	while(split)
	{
		split = strtok(nullptr, delimeter);
		if (!split)
		{
			break;
		}

		splits.emplace(splits.end(), std::string(split));
	}

	return splits;
}

std::string Utils::getFileSHA256(const char *filePath)
{
	std::ifstream fs(filePath, std::ios::binary);
	if (!fs.is_open())
	{
		//TODO: Read more about error types in C++ :)
		throw std::runtime_error("Unable to read file!");
	}

	std::vector<unsigned char> bytes(std::istreambuf_iterator<char>(fs), {});
	unsigned char sha256Bytes[SHA256_DIGEST_LENGTH];

        static unsigned char* (*p_SHA256)(const unsigned char *d, size_t n, unsigned char *md) = nullptr;
        if (!p_SHA256)
        {
                void* handle = dlopen("libcrypto.so.1.1", RTLD_NOLOAD | RTLD_LAZY);
                if (!handle) handle = dlopen("libcrypto.so.1.0.0", RTLD_NOLOAD | RTLD_LAZY);
                if (!handle) handle = dlopen("libcrypto.so.3", RTLD_NOLOAD | RTLD_LAZY);
                if (!handle) handle = RTLD_DEFAULT;
                p_SHA256 = (unsigned char*(*)(const unsigned char*, size_t, unsigned char*))dlsym(handle, "SHA256");
        }
        
        if (p_SHA256)
                p_SHA256(bytes.data(), bytes.size(), sha256Bytes);
        else
                memset(sha256Bytes, 0, SHA256_DIGEST_LENGTH); // fallback if not found

	std::stringstream sha256;
	for(int i = 0; i < SHA256_DIGEST_LENGTH; i++)
	{
		sha256 << std::hex << std::setw(2) << std::setfill('0') << (int)sha256Bytes[i];
	}

	fs.close();
	return sha256.str();
}

