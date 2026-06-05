#pragma once

#include <string>

namespace DX9Compat
{
	bool IsDX9Source(const std::string& source);
	std::string Transform(const std::string& source, const std::string& effectName);
}
