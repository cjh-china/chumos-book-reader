#include "parser.h"

bool Parser::parsePDF(const std::string& filename, Book& book) {
    book.title = filename;
    book.author = "PDF (minimal parser)";
    book.content.push_back("PDF format requires specialized parsing libraries.");
    book.content.push_back("Install Poppler, MuPDF, or pdfium for full support.");
    return true;
}
