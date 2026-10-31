#ifndef PARSER_H
#define PARSER_H

#include "reader.h"
#include <string>

class Parser {
public:
    static bool parse(const std::string& filename, Book& book);
    
private:
    static std::string getExtension(const std::string& filename);
    static bool parseTXT(const std::string& filename, Book& book);
    static bool parseEPUB(const std::string& filename, Book& book);
    static bool parseDOCX(const std::string& filename, Book& book);
    static bool parsePDF(const std::string& filename, Book& book);
};

#endif
