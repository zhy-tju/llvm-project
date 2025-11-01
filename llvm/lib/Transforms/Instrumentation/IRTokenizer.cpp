#include "llvm/Transforms/Instrumentation/IRTokenizer.h"

// 包含所有必要的C++标准库头文件
#include <regex>
#include <fstream>
#include <sstream>
#include <iostream>
#include <cmath>
#include <cstring>
#include <limits>
#include <algorithm> // for std::max

// 引入 nlohmann/json 单头库（放在 llvm/include/llvm/Transforms/Instrumentation/json.hpp）
#include "llvm/Transforms/Instrumentation/json.hpp"
using json = nlohmann::json;


namespace IRTokenizer {

// --- 构造函数和加载逻辑 ---
LLVMIRTokenizer::LLVMIRTokenizer(const std::string& knownTokensJsonPath, const std::string& vocabJsonPath) {
    // 1. 加载 Known Tokens
    std::ifstream knownIfs(knownTokensJsonPath);
    if (!knownIfs.is_open()) {
        std::cerr << "警告：无法打开已知Token文件 '" << knownTokensJsonPath << "'。\n";
    } else {
        try {
            json knownData = json::parse(knownIfs);
            if (knownData.contains("KNOWN_TOKENS") && knownData["KNOWN_TOKENS"].is_array()) {
                for (const auto& token : knownData["KNOWN_TOKENS"]) {
                    if (token.is_string()) {
                        knownTokens.insert(token.get<std::string>());
                    }
                }
                std::cout << "信息：成功加载 " << knownTokens.size() << " 个已知Token。\n";
            } else {
                 std::cerr << "警告：已知Token文件 '" << knownTokensJsonPath << "' 格式不正确。\n";
            }
        } catch (const json::parse_error& e) { // 更具体地捕获JSON异常
             std::cerr << "警告：解析已知Token文件 '" << knownTokensJsonPath << "' 时出错: " << e.what() << "\n";
        } catch (const std::exception& e) {
             std::cerr << "警告：加载已知Token文件时发生异常: " << e.what() << "\n";
        }
    }

    // 2. 加载 Vocab Map
    std::ifstream vocabIfs(vocabJsonPath);
    if (!vocabIfs.is_open()) {
         std::cerr << "错误：无法打开词汇表文件 '" << vocabJsonPath << "'！分词器将无法工作。\n";
         return;
    }
    try {
        json vocabData = json::parse(vocabIfs);
        if (vocabData.is_object()) {
            vocabMap = vocabData.get<std::map<std::string, int>>();
             std::cout << "信息：成功加载词汇表，大小: " << vocabMap.size() << "\n";
             clsId = getTokenId("[CLS]");
             sepId = getTokenId("[SEP]");
             padId = getTokenId("[PAD]");
             unkId = getTokenId("[UNK]");
             if (clsId == -1 || sepId == -1 || padId == -1 || unkId == -1) {
                  std::cerr << "错误：词汇表中缺少必要的特殊Token！\n";
                  vocabMap.clear();
             }
        } else {
             std::cerr << "错误：词汇表文件 '" << vocabJsonPath << "' 顶层必须是一个JSON对象。\n";
        }
    } catch (const json::parse_error& e) {
         std::cerr << "错误：解析词汇表文件 '" << vocabJsonPath << "' 时出错: " << e.what() << "\n";
    } catch (const std::exception& e) {
         std::cerr << "错误：加载词汇表文件时发生异常: " << e.what() << "\n";
    }
}

// 辅助函数：查找Token ID，带 [UNK] 回退
int LLVMIRTokenizer::getTokenId(const std::string& token) const {
    auto it = vocabMap.find(token);
    if (it != vocabMap.end()) {
        return it->second;
    }
    // 确保 unkId 有效后再返回
    auto unkIt = vocabMap.find("[UNK]");
    if (unkIt != vocabMap.end()) {
        return unkIt->second;
    }
    return -1; // 严重错误：连 [UNK] 都没有
}


// --- 核心分词逻辑 (rawTokenize, abstractTokens, etc.) ---
std::vector<RawToken> LLVMIRTokenizer::rawTokenize(const std::string& instructionString) {
     const std::vector<std::pair<std::regex, std::string>> patterns = {
        {std::regex(R"([%@]\s*\"[^\"]+\")"), "QUOTED_ID"}, {std::regex(R"(#\d+)"), "ATTRIBUTE_GROUP"},
        {std::regex(R"(![a-zA-Z0-9_.-]+)"), "METADATA_ID"}, {std::regex(R"(!\d+)"), "METADATA_NODE"},
        {std::regex(R"([%@][a-zA-Z0-9_$.?<>:-]+)"), "IDENTIFIER"}, {std::regex(R"([a-zA-Z0-9_$.]+:)"), "LABEL_DEF"},
        {std::regex(R"(::)"), "SCOPE_OP"}, {std::regex(R"(\b[a-zA-Z_][a-zA-Z0-9._<>]*\b)"), "KEYWORD_LIKE"},
        {std::regex(R"(0x[HKLMNR]?[0-9a-fA-F]+)"), "HEX_LITERAL"}, {std::regex(R"([-+]?\d+\.\d*([eE][-+]?\d+)?)"), "FLOAT_LITERAL"},
        {std::regex(R"([-]?\d+)"), "INT_LITERAL"}, {std::regex(R"([\{\}\(\)\[\]<>,=*|])"), "PUNCTUATION"},
        {std::regex(R"(\*+)"), "POINTER"}, {std::regex(R"(\s+)"), "WHITESPACE"}, {std::regex(R"(.)"), "MISMATCH"}
    };
    std::vector<RawToken> tokens; std::string remainingString = instructionString; std::smatch match;
    while (!remainingString.empty()) {
        bool matched = false;
        for (const auto& pair : patterns) {
            try { // 添加 try-catch 块以捕获可能的 regex 异常
                if (std::regex_search(remainingString, match, pair.first, std::regex_constants::match_continuous)) {
                    std::string value = match.str(0); std::string type = pair.second;
                    if (type != "WHITESPACE") tokens.push_back({value, type});
                    remainingString = match.suffix().str(); matched = true; break;
                }
            } catch (const std::regex_error& e) {
                std::cerr << "正则表达式错误: " << e.what() << " Code: " << e.code() << std::endl;
                // 发生错误时，跳过当前字符，避免无限循环
                remainingString = remainingString.substr(1);
                matched = true; // 标记为已处理，即使是错误处理
                break;
            }
        }
        if (!matched && !remainingString.empty()) {
             std::cerr << "警告：原始分词无法匹配: '" << remainingString[0] << "' in \"" << instructionString << "\"\n";
             tokens.push_back({remainingString.substr(0, 1), "MISMATCH"});
             remainingString = remainingString.substr(1);
        }
    } return tokens;
}

std::optional<double> LLVMIRTokenizer::parseLLVMFloatLiteral(const std::string& token) {
     try { size_t processed = 0; double val = std::stod(token, &processed); if (processed == token.length()) return val; } catch (...) {}
    if (token.rfind("0x", 0) == 0 && token.length() == 18) { try { uint64_t i = std::stoull(token.substr(2), nullptr, 16); double d; std::memcpy(&d, &i, sizeof(d)); return d; } catch (...) {} }
    return std::nullopt;
}

std::string LLVMIRTokenizer::normalizeIdentifier(const std::string& token) {
     size_t lastDot = token.find_last_of('.'); if (lastDot != std::string::npos && lastDot < token.length() - 1) { bool allDigits = true; for (size_t i = lastDot + 1; i < token.length(); ++i) if (!std::isdigit(static_cast<unsigned char>(token[i]))) { allDigits = false; break; } if (allDigits) return token.substr(0, lastDot); } return token;
}

std::vector<std::string> LLVMIRTokenizer::splitQuotedIdentifier(const std::string& content) {
    std::vector<std::string> subs; std::string cur; for (size_t i=0; i<content.length(); ++i) { char cr = content[i]; unsigned char c = static_cast<unsigned char>(cr); if (cr==':' && i+1<content.length() && content[i+1]==':') { if (!cur.empty()) subs.push_back(cur); subs.push_back("::"); cur=""; i++; } else if (cr=='.' || cr=='<' || cr=='>' || cr==',' || std::isspace(c)) { if (!cur.empty()) subs.push_back(cur); if (!std::isspace(c)) subs.push_back(std::string(1, cr)); cur=""; } else cur+=cr; } if (!cur.empty()) subs.push_back(cur); return subs;
}

std::vector<std::string> LLVMIRTokenizer::abstractTokens(const std::vector<RawToken>& rawTokens) {
    std::vector<std::string> absTs; for(const auto& rt : rawTokens){ const std::string& tok=rt.Value; const std::string& typ=rt.Type; if (typ=="QUOTED_ID"){ std::string ct=tok.substr(1); if(!ct.empty() && ct.front()==' ') ct=ct.substr(1); if(ct.length()>=2 && ct.front()=='"' && ct.back()=='"') ct=ct.substr(1, ct.length()-2); std::vector<std::string> subs=splitQuotedIdentifier(ct); for(const std::string& st : subs){ if(st=="::")absTs.push_back("[SCOPE]"); else if(st=="<")absTs.push_back("[T_BEGIN]"); else if(st==">")absTs.push_back("[T_END]"); else if(st==",")absTs.push_back(","); else if(knownTokens.count(st))absTs.push_back(st); else {std::string n=normalizeIdentifier(st); if(knownTokens.count(n))absTs.push_back(n); else absTs.push_back("[IDENTIFIER_UNK]");}}}
    else if(tok.rfind("%",0)==0)absTs.push_back("[ID]"); else if(tok.rfind("@llvm.",0)==0)absTs.push_back("[INTRINSIC]"); else if(tok.rfind("@",0)==0){std::string n=normalizeIdentifier(tok); if(knownTokens.count(n))absTs.push_back(n); else absTs.push_back("[IDENTIFIER_UNK]");}
    else if(!tok.empty() && tok.back()==':')absTs.push_back("[LABEL]"); else if(typ=="ATTRIBUTE_GROUP")absTs.push_back("[ATTR_GROUP]"); else if(typ=="METADATA_ID"||typ=="METADATA_NODE"){std::string ct=tok.substr(1); if(knownTokens.count(ct))absTs.push_back(ct); else absTs.push_back("[METADATA]");}
    else if(typ=="INT_LITERAL"){if(tok=="0")absTs.push_back("[ZERO]"); else if(tok=="1")absTs.push_back("[ONE]"); else absTs.push_back("[NUM]");}
    else if(typ=="FLOAT_LITERAL"||typ=="HEX_LITERAL"){std::optional<double> vO=parseLLVMFloatLiteral(tok); if(vO){double v=*vO; constexpr double eps=1e-9; /* 使用稍大的epsilon */ if(std::abs(v-0.0)<eps)absTs.push_back("[FP_ZERO]"); else if(std::abs(v-1.0)<eps)absTs.push_back("[FP_ONE]"); else if(std::abs(v-(-1.0))<eps)absTs.push_back("[FP_NEG_ONE]"); else if(std::abs(v)<1.0 && std::abs(v)>eps)absTs.push_back("[FP_SMALL]"); else if(v>0.0)absTs.push_back("[FP_POS]"); else if(v<0.0)absTs.push_back("[FP_NEG]"); else absTs.push_back("[FP_NUM_GENERIC]");} else absTs.push_back("[FP_NUM_GENERIC]");}
    else if(typ=="MISMATCH"){if(knownTokens.count(tok))absTs.push_back(tok); else absTs.push_back("[UNK]");}
    else{std::string n=normalizeIdentifier(tok); if(knownTokens.count(n))absTs.push_back(n); else { if(typ=="PUNCTUATION"||typ=="POINTER"||typ=="SCOPE_OP"){if(knownTokens.count(tok))absTs.push_back(tok); else absTs.push_back("[IDENTIFIER_UNK]");} else absTs.push_back("[IDENTIFIER_UNK]");}}}
    return absTs;
}

std::vector<std::string> LLVMIRTokenizer::processBasicBlock(const std::string& blockString) {
     std::vector<std::string> allAbsTs; std::stringstream ss(blockString); std::string ln; bool firstLn=true; while(std::getline(ss,ln)){ size_t f=ln.find_first_not_of(" \t\n\r"); if(f==std::string::npos) continue; size_t l=ln.find_last_not_of(" \t\n\r"); std::string trmLn=ln.substr(f,(l-f+1)); if(trmLn.empty()) continue; if(!firstLn)allAbsTs.push_back(EOL_TOKEN); std::vector<RawToken> raw=rawTokenize(trmLn); std::vector<std::string> abs=abstractTokens(raw); allAbsTs.insert(allAbsTs.end(),abs.begin(),abs.end()); firstLn=false; } return allAbsTs;
}

// --- prepareForModel 实现 ---
ModelInput LLVMIRTokenizer::prepareForModel(
    const std::string& blockString,
    int maxLength,
    const std::string& truncationStrategy
) {
    ModelInput result;

    if (vocabMap.empty() || clsId == -1) {
        std::cerr << "错误：词汇表未正确加载！\n";
        return result;
    }

    // 1. 获取抽象Token序列
    std::vector<std::string> tokens = processBasicBlock(blockString);
    result.original_token_len = tokens.size();

    // 2. 将Tokens转换为IDs
    std::vector<int> encoded_ids;
    encoded_ids.reserve(tokens.size());
    for (const std::string& t : tokens) {
        encoded_ids.push_back(getTokenId(t));
    }

    // 3. 截断
    int maxTokensInside = maxLength - 2;
    if (maxTokensInside < 0) maxTokensInside = 0;

    if (encoded_ids.size() > static_cast<size_t>(maxTokensInside)) {
        if (truncationStrategy == "truncate_head") {
            encoded_ids.assign(encoded_ids.end() - maxTokensInside, encoded_ids.end());
        } else {
            encoded_ids.resize(maxTokensInside);
        }
    }

    // 4. 添加特殊Tokens
    result.input_ids.reserve(maxLength);
    result.input_ids.push_back(clsId);
    result.input_ids.insert(result.input_ids.end(), encoded_ids.begin(), encoded_ids.end());
    result.input_ids.push_back(sepId);

    // 5. 填充和生成Attention Mask
    int currentLength = result.input_ids.size();
    result.attention_mask.assign(currentLength, 1);

    int paddingLength = maxLength - currentLength;
    if (paddingLength > 0) {
        result.input_ids.insert(result.input_ids.end(), paddingLength, padId);
        result.attention_mask.insert(result.attention_mask.end(), paddingLength, 0);
    }
    if (result.input_ids.size() > static_cast<size_t>(maxLength)) {
        result.input_ids.resize(maxLength);
        result.attention_mask.resize(maxLength);
    }

    return result;
}

} // namespace IRTokenizer
