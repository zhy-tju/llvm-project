#ifndef LLVM_IR_TOKENIZER_H
#define LLVM_IR_TOKENIZER_H

#include <string>
#include <vector>
#include <set>
#include <optional>
#include <map> // 用于存储词汇表

// 定义我们将要使用的命名空间
namespace IRTokenizer {

// 定义代表指令结束的特殊Token
const std::string EOL_TOKEN = "[EOL]";

// 用于存储原始分词结果的结构体
struct RawToken {
    std::string Value;
    std::string Type;
};

// 用于返回模型输入的结构体 (已简化)
struct ModelInput {
    std::vector<int> input_ids;      // 最终的输入ID序列
    std::vector<int> attention_mask; // 对应的Attention Mask
    int original_token_len = 0;    // 【新增】原始Token序列的长度 (截断前)
};


class LLVMIRTokenizer {
public:
    // 构造函数：需要已知Token JSON路径 和 词汇表JSON路径
    LLVMIRTokenizer(const std::string& knownTokensJsonPath, const std::string& vocabJsonPath);

    // 核心功能：将单个LLVM IR指令字符串转换为抽象Token列表
    std::vector<std::string> tokenizeInstruction(const std::string& instructionString);

    // 辅助功能：将整个基本块（可能包含多行）转换为抽象Token列表
    std::vector<std::string> processBasicBlock(const std::string& blockString);

    // 【核心修改】将基本块字符串准备成模型输入格式 (已移除 multi_windows)
    ModelInput prepareForModel(
        const std::string& blockString,
        int maxLength = 256,
        const std::string& truncationStrategy = "truncate_head" // "truncate_head" 或 "truncate_tail"
    );

private:
    // 私有成员变量
    std::set<std::string> knownTokens;
    std::map<std::string, int> vocabMap;
    int clsId = -1, sepId = -1, padId = -1, unkId = -1;

    // 私有辅助函数
    std::vector<RawToken> rawTokenize(const std::string& instructionString);
    std::vector<std::string> abstractTokens(const std::vector<RawToken>& rawTokens);
    std::optional<double> parseLLVMFloatLiteral(const std::string& token);
    std::string normalizeIdentifier(const std::string& token);
    std::vector<std::string> splitQuotedIdentifier(const std::string& quotedIdContent);
    int getTokenId(const std::string& token) const;
};

} // namespace IRTokenizer

#endif // LLVM_IR_TOKENIZER_H