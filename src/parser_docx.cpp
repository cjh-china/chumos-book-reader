#include "parser.h"

bool Parser::parseDOCX(const std::string& filename, Book& book) {
    book.title = filename;
    book.author = "DOCX (minimal parser)";
    book.content.push_back("DOCX format support requires ZIP extraction and XML parsing.");
    book.content.push_back("Install minizip or libzip for full support.");
    return true;
}
