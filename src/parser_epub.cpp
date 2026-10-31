#include "parser.h"

bool Parser::parseEPUB(const std::string& filename, Book& book) {
    book.title = filename;
    book.author = "EPUB (minimal parser)";
    book.content.push_back("EPUB format support requires ZIP extraction.");
    book.content.push_back("Install minizip or libzip for full support.");
    return true;
}
