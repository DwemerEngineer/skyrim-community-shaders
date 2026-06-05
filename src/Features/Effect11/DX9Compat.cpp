#include "DX9Compat.h"

#include <algorithm>
#include <cctype>
#include <sstream>
#include <unordered_map>
#include <vector>

namespace DX9Compat
{
	static bool IsIdentChar(char c)
	{
		return std::isalnum(static_cast<unsigned char>(c)) || c == '_';
	}

	static bool IsWordBoundary(const std::string& s, size_t pos, size_t len)
	{
		if (pos > 0 && IsIdentChar(s[pos - 1]))
			return false;
		size_t end = pos + len;
		if (end < s.size() && IsIdentChar(s[end]))
			return false;
		return true;
	}

	static size_t FindWord(const std::string& s, const std::string& word, size_t start = 0)
	{
		size_t pos = start;
		while ((pos = s.find(word, pos)) != std::string::npos) {
			if (IsWordBoundary(s, pos, word.size()))
				return pos;
			pos += word.size();
		}
		return std::string::npos;
	}

	static void ReplaceWord(std::string& s, const std::string& from, const std::string& to)
	{
		size_t pos = 0;
		while ((pos = FindWord(s, from, pos)) != std::string::npos) {
			s.replace(pos, from.size(), to);
			pos += to.size();
		}
	}

	static size_t FindMatchingBrace(const std::string& s, size_t openPos)
	{
		int depth = 1;
		for (size_t i = openPos + 1; i < s.size(); ++i) {
			if (s[i] == '{')
				++depth;
			else if (s[i] == '}') {
				if (--depth == 0)
					return i;
			}
		}
		return std::string::npos;
	}

	static size_t FindMatchingParen(const std::string& s, size_t openPos)
	{
		int depth = 1;
		for (size_t i = openPos + 1; i < s.size(); ++i) {
			if (s[i] == '(')
				++depth;
			else if (s[i] == ')') {
				if (--depth == 0)
					return i;
			}
		}
		return std::string::npos;
	}

	static size_t SplitAtComma(const std::string& s, size_t start, size_t end)
	{
		int depth = 0;
		for (size_t i = start; i < end; ++i) {
			if (s[i] == '(' || s[i] == '{')
				++depth;
			else if (s[i] == ')' || s[i] == '}')
				--depth;
			else if (s[i] == ',' && depth == 0)
				return i;
		}
		return std::string::npos;
	}

	static std::string Trim(const std::string& s)
	{
		size_t start = s.find_first_not_of(" \t\r\n");
		if (start == std::string::npos)
			return {};
		size_t end = s.find_last_not_of(" \t\r\n");
		return s.substr(start, end - start + 1);
	}

	static std::string ToUpper(const std::string& s)
	{
		std::string r = s;
		for (auto& c : r)
			c = static_cast<char>(std::toupper(static_cast<unsigned char>(c)));
		return r;
	}

	static bool CaseInsensitiveEquals(const std::string& a, const std::string& b)
	{
		if (a.size() != b.size())
			return false;
		for (size_t i = 0; i < a.size(); ++i)
			if (std::toupper(static_cast<unsigned char>(a[i])) != std::toupper(static_cast<unsigned char>(b[i])))
				return false;
		return true;
	}

	struct DX9SamplerInfo
	{
		std::string samplerName;
		std::string textureName;
		std::string minFilter;
		std::string magFilter;
		std::string mipFilter;
		std::string addressU;
		std::string addressV;
		size_t blockStart = 0;
		size_t blockEnd = 0;
	};

	static std::string ComputeDX11Filter(const std::string& minF, const std::string& magF, const std::string& mipF)
	{
		auto min = ToUpper(Trim(minF));
		auto mag = ToUpper(Trim(magF));
		auto mip = ToUpper(Trim(mipF));

		bool minPoint = (min == "POINT" || min.empty());
		bool magPoint = (mag == "POINT" || mag.empty());
		bool mipNone = (mip == "NONE" || mip.empty());
		bool mipLinear = (mip == "LINEAR");

		if (minPoint && magPoint)
			return mipLinear ? "MIN_MAG_POINT_MIP_LINEAR" : "MIN_MAG_MIP_POINT";
		if (!minPoint && !magPoint)
			return (mipNone || mip == "POINT") ? "MIN_MAG_LINEAR_MIP_POINT" : "MIN_MAG_MIP_LINEAR";

		return "MIN_MAG_MIP_LINEAR";
	}

	static std::string NormalizeAddressMode(const std::string& mode)
	{
		auto upper = ToUpper(Trim(mode));
		if (upper == "CLAMP" || upper == "CLAMP")
			return "Clamp";
		if (upper == "WRAP")
			return "Wrap";
		if (upper == "MIRROR")
			return "Mirror";
		if (upper == "BORDER")
			return "Border";
		return "Clamp";
	}

	static std::string ExtractSamplerStateProperty(const std::string& block, const std::string& propName)
	{
		size_t pos = 0;
		while (pos < block.size()) {
			pos = block.find(propName, pos);
			if (pos == std::string::npos)
				break;
			if (pos > 0 && IsIdentChar(block[pos - 1])) {
				pos += propName.size();
				continue;
			}
			size_t afterProp = pos + propName.size();
			size_t eq = block.find('=', afterProp);
			if (eq == std::string::npos)
				break;
			size_t valStart = block.find_first_not_of(" \t", eq + 1);
			if (valStart == std::string::npos)
				break;
			size_t valEnd = block.find_first_of(";\r\n", valStart);
			if (valEnd == std::string::npos)
				valEnd = block.size();
			return Trim(block.substr(valStart, valEnd - valStart));
		}
		return {};
	}

	static std::vector<DX9SamplerInfo> ParseSamplerStateBlocks(const std::string& source)
	{
		std::vector<DX9SamplerInfo> result;

		size_t pos = 0;
		while (pos < source.size()) {
			pos = source.find("sampler2D", pos);
			if (pos == std::string::npos)
				break;
			if (!IsWordBoundary(source, pos, 9)) {
				pos += 9;
				continue;
			}

			size_t nameStart = source.find_first_not_of(" \t", pos + 9);
			if (nameStart == std::string::npos) {
				pos += 9;
				continue;
			}
			size_t nameEnd = nameStart;
			while (nameEnd < source.size() && IsIdentChar(source[nameEnd]))
				++nameEnd;

			std::string samplerName = source.substr(nameStart, nameEnd - nameStart);

			size_t afterName = source.find_first_not_of(" \t\r\n", nameEnd);
			if (afterName == std::string::npos || source[afterName] != '=') {
				DX9SamplerInfo info;
				info.samplerName = samplerName;
				info.blockStart = pos;
				size_t semi = source.find(';', nameEnd);
				info.blockEnd = (semi != std::string::npos) ? semi + 1 : nameEnd;
				result.push_back(std::move(info));
				pos = info.blockEnd;
				continue;
			}

			size_t afterEq = source.find_first_not_of(" \t\r\n", afterName + 1);
			if (afterEq == std::string::npos || source.compare(afterEq, 13, "sampler_state") != 0) {
				pos = nameEnd;
				continue;
			}

			size_t braceOpen = source.find('{', afterEq + 13);
			if (braceOpen == std::string::npos) {
				pos = afterEq + 13;
				continue;
			}

			size_t braceClose = FindMatchingBrace(source, braceOpen);
			if (braceClose == std::string::npos) {
				pos = braceOpen + 1;
				continue;
			}

			size_t blockEnd = braceClose + 1;
			if (blockEnd < source.size() && source[blockEnd] == ';')
				++blockEnd;

			std::string block = source.substr(braceOpen, braceClose - braceOpen + 1);

			DX9SamplerInfo info;
			info.samplerName = samplerName;
			info.blockStart = pos;
			info.blockEnd = blockEnd;

			auto texProp = ExtractSamplerStateProperty(block, "Texture");
			if (!texProp.empty()) {
				size_t lt = texProp.find('<');
				size_t gt = texProp.find('>');
				if (lt != std::string::npos && gt != std::string::npos && gt > lt)
					info.textureName = Trim(texProp.substr(lt + 1, gt - lt - 1));
			}

			info.minFilter = ExtractSamplerStateProperty(block, "MinFilter");
			info.magFilter = ExtractSamplerStateProperty(block, "MagFilter");
			info.mipFilter = ExtractSamplerStateProperty(block, "MipFilter");
			info.addressU = ExtractSamplerStateProperty(block, "AddressU");
			info.addressV = ExtractSamplerStateProperty(block, "AddressV");

			result.push_back(std::move(info));
			pos = blockEnd;
		}

		return result;
	}

	static void StripAsmBlocks(std::string& source)
	{
		size_t pos = 0;
		while ((pos = FindWord(source, "asm", pos)) != std::string::npos) {
			size_t braceOpen = source.find('{', pos + 3);
			if (braceOpen == std::string::npos) {
				pos += 3;
				continue;
			}

			bool onlyWhitespace = true;
			for (size_t i = pos + 3; i < braceOpen; ++i) {
				if (!std::isspace(static_cast<unsigned char>(source[i]))) {
					onlyWhitespace = false;
					break;
				}
			}
			if (!onlyWhitespace) {
				pos += 3;
				continue;
			}

			size_t braceClose = FindMatchingBrace(source, braceOpen);
			if (braceClose == std::string::npos) {
				pos += 3;
				continue;
			}

			size_t end = braceClose + 1;
			if (end < source.size() && source[end] == ';')
				++end;

			size_t stripStart = pos;
			size_t scanBack = pos;
			while (scanBack > 0 && std::isspace(static_cast<unsigned char>(source[scanBack - 1])))
				--scanBack;
			if (scanBack > 0 && source[scanBack - 1] == '=') {
				--scanBack;
				while (scanBack > 0 && std::isspace(static_cast<unsigned char>(source[scanBack - 1])))
					--scanBack;
				size_t kwEnd = scanBack;
				while (scanBack > 0 && IsIdentChar(source[scanBack - 1]))
					--scanBack;
				std::string kw = source.substr(scanBack, kwEnd - scanBack);
				if (kw == "PixelShader" || kw == "VertexShader")
					stripStart = scanBack;
			}

			std::string replacement = "/* asm block removed */";
			source.replace(stripStart, end - stripStart, replacement);
			pos = stripStart + replacement.size();
		}
	}

	static void RewriteTextureDeclarations(std::string& source)
	{
		size_t pos = 0;
		while ((pos = FindWord(source, "texture2D", pos)) != std::string::npos) {
			source.replace(pos, 9, "Texture2D");
			pos += 9;
		}

		pos = 0;
		while (pos < source.size()) {
			pos = FindWord(source, "texture", pos);
			if (pos == std::string::npos)
				break;
			if (source.compare(pos, 9, "Texture2D") == 0) {
				pos += 9;
				continue;
			}
			size_t afterKw = source.find_first_not_of(" \t", pos + 7);
			if (afterKw != std::string::npos && IsIdentChar(source[afterKw])) {
				size_t nameEnd = afterKw;
				while (nameEnd < source.size() && IsIdentChar(source[nameEnd]))
					++nameEnd;
				size_t afterName = source.find_first_not_of(" \t", nameEnd);
				if (afterName != std::string::npos && source[afterName] == ';') {
					source.replace(pos, 7, "Texture2D");
					pos += 9;
					continue;
				}
			}
			pos += 7;
		}
	}

	static void RewriteSamplerDeclarations(std::string& source, const std::vector<DX9SamplerInfo>& samplers)
	{
		for (auto it = samplers.rbegin(); it != samplers.rend(); ++it) {
			auto& info = *it;
			std::string filter = ComputeDX11Filter(info.minFilter, info.magFilter, info.mipFilter);
			std::string addrU = NormalizeAddressMode(info.addressU);
			std::string addrV = NormalizeAddressMode(info.addressV);

			std::string replacement;
			if (!info.minFilter.empty() || !info.addressU.empty()) {
				replacement = "SamplerState " + info.samplerName + "\n{\n";
				replacement += "\tFilter = " + filter + ";\n";
				replacement += "\tAddressU = " + addrU + ";\n";
				replacement += "\tAddressV = " + addrV + ";\n";
				replacement += "};";
			} else {
				replacement = "SamplerState " + info.samplerName + " {};";
			}

			source.replace(info.blockStart, info.blockEnd - info.blockStart, replacement);
		}
	}

	static void RewriteTexSamplingCalls(std::string& source,
		const std::unordered_map<std::string, std::string>& samplerToTexture,
		const std::string& funcName,
		bool isLod)
	{
		size_t pos = 0;
		while (pos < source.size()) {
			pos = source.find(funcName, pos);
			if (pos == std::string::npos)
				break;
			if (!IsWordBoundary(source, pos, funcName.size())) {
				pos += funcName.size();
				continue;
			}

			size_t parenOpen = source.find('(', pos + funcName.size());
			if (parenOpen == std::string::npos) {
				pos += funcName.size();
				continue;
			}

			bool onlyWhitespace = true;
			for (size_t i = pos + funcName.size(); i < parenOpen; ++i) {
				if (!std::isspace(static_cast<unsigned char>(source[i]))) {
					onlyWhitespace = false;
					break;
				}
			}
			if (!onlyWhitespace) {
				pos += funcName.size();
				continue;
			}

			size_t parenClose = FindMatchingParen(source, parenOpen);
			if (parenClose == std::string::npos) {
				pos += funcName.size();
				continue;
			}

			std::string args = source.substr(parenOpen + 1, parenClose - parenOpen - 1);

			size_t commaPos = SplitAtComma(args, 0, args.size());
			if (commaPos == std::string::npos) {
				pos = parenClose + 1;
				continue;
			}

			std::string samplerArg = Trim(args.substr(0, commaPos));
			std::string coordArg = Trim(args.substr(commaPos + 1));

			auto texIt = samplerToTexture.find(samplerArg);
			std::string texName;
			if (texIt != samplerToTexture.end()) {
				texName = texIt->second;
			} else {
				texName = samplerArg;
			}

			std::string replacement;
			if (isLod) {
				replacement = texName + ".SampleLevel(" + samplerArg + ", (" + coordArg + ").xy, (" + coordArg + ").w)";
			} else {
				replacement = texName + ".Sample(" + samplerArg + ", " + coordArg + ")";
			}

			source.replace(pos, parenClose - pos + 1, replacement);
			pos += replacement.size();
		}
	}

	static void RewriteTechniques(std::string& source)
	{
		size_t pos = 0;
		while (pos < source.size()) {
			pos = FindWord(source, "technique", pos);
			if (pos == std::string::npos)
				break;
			if (pos + 9 < source.size() && source.compare(pos, 11, "technique11") == 0) {
				pos += 11;
				continue;
			}
			source.replace(pos, 9, "technique11");
			pos += 11;
		}
	}

	static void RewriteCompileTargets(std::string& source)
	{
		auto replaceTarget = [&](const std::string& prefix) {
			size_t pos = 0;
			while ((pos = source.find(prefix, pos)) != std::string::npos) {
				size_t verStart = pos + prefix.size();
				if (verStart + 2 >= source.size() || source[verStart + 1] != '_') {
					pos = verStart;
					continue;
				}
				char major = source[verStart];
				if (major >= '1' && major <= '4') {
					size_t verEnd = verStart;
					while (verEnd < source.size() && (std::isdigit(static_cast<unsigned char>(source[verEnd])) || source[verEnd] == '_'))
						++verEnd;
					source.replace(verStart, verEnd - verStart, "5_0");
					pos = verStart + 3;
				} else {
					pos = verStart;
				}
			}
		};

		replaceTarget("compile vs_");
		replaceTarget("compile ps_");
	}

	static void RewriteSemantics(std::string& source)
	{
		auto replaceSemantic = [&](const std::string& from, const std::string& to) {
			size_t pos = 0;
			while (pos < source.size()) {
				pos = source.find(from, pos);
				if (pos == std::string::npos)
					break;
				if (pos > 0 && source[pos - 1] != ':' && !std::isspace(static_cast<unsigned char>(source[pos - 1]))) {
					pos += from.size();
					continue;
				}
				size_t afterSem = pos + from.size();
				if (afterSem < source.size() && IsIdentChar(source[afterSem])) {
					pos += from.size();
					continue;
				}
				if (pos >= 2) {
					size_t colonPos = source.rfind(':', pos - 1);
					if (colonPos != std::string::npos) {
						bool onlySpaces = true;
						for (size_t i = colonPos + 1; i < pos; ++i) {
							if (!std::isspace(static_cast<unsigned char>(source[i]))) {
								onlySpaces = false;
								break;
							}
						}
						if (onlySpaces) {
							source.replace(pos, from.size(), to);
							pos += to.size();
							continue;
						}
					}
				}
				pos += from.size();
			}
		};

		replaceSemantic("POSITION0", "SV_POSITION");
		replaceSemantic("POSITION", "SV_POSITION");

		for (int i = 7; i >= 0; --i) {
			replaceSemantic("COLOR" + std::to_string(i), "SV_TARGET" + std::to_string(i));
		}
		replaceSemantic("COLOR", "SV_TARGET");
	}

	static bool IsInsideLineComment(const std::string& source, size_t pos)
	{
		size_t lineStart = source.rfind('\n', pos);
		if (lineStart == std::string::npos)
			lineStart = 0;
		else
			++lineStart;
		size_t commentPos = source.find("//", lineStart);
		return commentPos != std::string::npos && commentPos < pos;
	}

	static void HandleVPOS(std::string& source)
	{
		size_t pos = 0;
		while (pos < source.size()) {
			pos = source.find("VPOS", pos);
			if (pos == std::string::npos)
				break;
			if (!IsWordBoundary(source, pos, 4) || IsInsideLineComment(source, pos)) {
				pos += 4;
				continue;
			}

			size_t colonSearch = pos;
			while (colonSearch > 0) {
				--colonSearch;
				char c = source[colonSearch];
				if (c == ':')
					break;
				if (!std::isspace(static_cast<unsigned char>(c))) {
					colonSearch = 0;
					break;
				}
			}
			if (colonSearch == 0 || source[colonSearch] != ':') {
				pos += 4;
				continue;
			}

			size_t typeEnd = colonSearch;
			while (typeEnd > 0 && std::isspace(static_cast<unsigned char>(source[typeEnd - 1])))
				--typeEnd;
			size_t nameEnd = typeEnd;
			while (nameEnd > 0 && IsIdentChar(source[nameEnd - 1]))
				--nameEnd;
			std::string paramName(source, nameEnd, typeEnd - nameEnd);

			size_t typeStart = nameEnd;
			while (typeStart > 0 && std::isspace(static_cast<unsigned char>(source[typeStart - 1])))
				--typeStart;
			while (typeStart > 0 && IsIdentChar(source[typeStart - 1]))
				--typeStart;

			std::string svParamName = "_" + paramName + "_dx9";
			std::string replFrom = source.substr(typeStart, pos + 4 - typeStart);
			std::string replTo = "float4 " + svParamName + " : SV_POSITION";
			source.replace(typeStart, pos + 4 - typeStart, replTo);

			size_t insertPos = typeStart + replTo.size();

			size_t funcBodyOpen = source.find('{', insertPos);
			if (funcBodyOpen != std::string::npos) {
				std::string injection = "\n\tfloat2 " + paramName + " = " + svParamName + ".xy;";
				source.insert(funcBodyOpen + 1, injection);
			}

			pos = insertPos;
		}
	}

	static void StripPassRenderStates(std::string& source)
	{
		static const std::vector<std::string> dx9States = {
			"SRGBWRITEENABLE", "SRGBWriteEnable",
			"SRGBTexture",
			"AlphaTestEnable",
			"AlphaBlendEnable",
			"ZEnable",
			"ZWriteEnable",
			"ColorWriteEnable",
			"CullMode",
			"SrcBlend",
			"DestBlend",
			"FogEnable",
			"StencilEnable",
			"StencilFunc",
			"StencilRef",
			"StencilPass",
			"StencilFail",
			"SEPARATEALPHABLENDENABLE",
			"DitherEnable",
			"MaxMipLevel",
			"MipMapLodBias",
		};

		std::string result;
		result.reserve(source.size());
		std::istringstream stream(source);
		std::string line;
		while (std::getline(stream, line)) {
			std::string trimmed = Trim(line);
			bool isState = false;
			for (auto& state : dx9States) {
				if (trimmed.size() >= state.size() &&
					CaseInsensitiveEquals(trimmed.substr(0, state.size()), state)) {
					size_t afterState = state.size();
					if (afterState < trimmed.size()) {
						char next = trimmed[afterState];
						if (next == '=' || next == ' ' || next == '\t') {
							isState = true;
							break;
						}
					}
				}
			}
			if (!isState) {
				result += line;
				result += '\n';
			}
		}
		source = std::move(result);

		// Also strip inline occurrences (e.g. "SRGBTexture=FALSE;" within single-line sampler_state blocks or macros)
		static const std::vector<std::string> inlineStrip = {
			"SRGBTexture", "MaxMipLevel", "MipMapLodBias", "DitherEnable"
		};
		for (auto& prop : inlineStrip) {
			size_t pos = 0;
			while ((pos = FindWord(source, prop, pos)) != std::string::npos) {
				size_t eqPos = source.find_first_not_of(" \t", pos + prop.size());
				if (eqPos != std::string::npos && source[eqPos] == '=') {
					size_t semiPos = source.find(';', eqPos);
					if (semiPos != std::string::npos) {
						size_t end = semiPos + 1;
						while (end < source.size() && (source[end] == ' ' || source[end] == '\t'))
							++end;
						source.erase(pos, end - pos);
						continue;
					}
				}
				pos += prop.size();
			}
		}
	}

	static void ApplyTextureNameMappings(std::string& source, const std::string& effectName)
	{
		static const std::unordered_map<std::string, std::unordered_map<std::string, std::string>> perEffectMappings = {
			{ "enbeffect.fx", {
				{ "texs0", "TextureColor" },
				{ "texs1", "TextureBloomVanilla" },
				{ "texs2", "TextureAdaptationVanilla" },
				{ "texs3", "TextureBloom" },
				{ "texs4", "TextureAdaptation" },
				{ "texs7", "TexturePalette" },
			}},
		};

		static const std::unordered_map<std::string, std::string> genericMappings = {
			{ "texColor", "TextureColor" },
			{ "texDepth", "TextureDepth" },
			{ "texNoise", "TextureNoise" },
			{ "texPalette", "TexturePalette" },
			{ "texFocus", "TextureFocus" },
			{ "texCurr", "TextureCurrent" },
			{ "texPrev", "TexturePrevious" },
			{ "texMask", "TextureMask" },
		};

		auto effectIt = perEffectMappings.find(effectName);
		if (effectIt != perEffectMappings.end()) {
			for (auto& [from, to] : effectIt->second)
				ReplaceWord(source, from, to);
		}

		for (auto& [from, to] : genericMappings)
			ReplaceWord(source, from, to);
	}

	bool IsDX9Source(const std::string& source)
	{
		if (FindWord(source, "sampler_state") != std::string::npos)
			return true;
		if (FindWord(source, "sampler2D") != std::string::npos && source.find("sampler_state") != std::string::npos)
			return true;
		if (source.find("compile vs_3_0") != std::string::npos || source.find("compile ps_3_0") != std::string::npos)
			return true;
		if (source.find("compile vs_2_0") != std::string::npos || source.find("compile ps_2_0") != std::string::npos)
			return true;
		return false;
	}

	std::string Transform(const std::string& source, const std::string& effectName)
	{
		std::string result = "#define ENB_DX11 1\n" + source;

		ApplyTextureNameMappings(result, effectName);
		StripAsmBlocks(result);

		auto samplers = ParseSamplerStateBlocks(result);

		std::unordered_map<std::string, std::string> samplerToTexture;
		for (auto& info : samplers) {
			if (!info.textureName.empty())
				samplerToTexture[info.samplerName] = info.textureName;
		}

		RewriteTextureDeclarations(result);
		RewriteSamplerDeclarations(result, samplers);
		RewriteTexSamplingCalls(result, samplerToTexture, "tex2Dlod", true);
		RewriteTexSamplingCalls(result, samplerToTexture, "tex2D", false);
		RewriteTechniques(result);
		RewriteCompileTargets(result);
		RewriteSemantics(result);
		HandleVPOS(result);
		StripPassRenderStates(result);

		logger::info("[DX9Compat] Transformed '{}': {} samplers, {} texture mappings",
			effectName, samplers.size(), samplerToTexture.size());

		return result;
	}
}
