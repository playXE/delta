#include "utility.h"
#include <algorithm>
#include <cctype>
#include <fstream>
#pragma warning(push, 0)
#include <llvm/Support/CommandLine.h>
#include <llvm/Support/ErrorOr.h>
#include <llvm/Support/FileSystem.h>
#include <llvm/Support/Program.h>
#pragma warning(pop)

using namespace delta;

std::string delta::readLineFromFile(SourceLocation location) {
    std::ifstream file(location.file);

    while (--location.line) {
        file.ignore(std::numeric_limits<std::streamsize>::max(), '\n');
    }

    std::string lineContent;
    std::getline(file, lineContent);
    return lineContent;
}

void delta::renameFile(llvm::Twine sourcePath, llvm::Twine targetPath) {
    auto permissions = llvm::sys::fs::getPermissions(sourcePath);
    if (auto error = permissions.getError()) {
        ABORT("couldn't get permissions for '" << sourcePath << "': " << error.message());
    }
    if (auto error = llvm::sys::fs::copy_file(sourcePath, targetPath)) {
        ABORT("couldn't copy '" << sourcePath << "' to '" << targetPath << "': " << error.message());
    }
    if (auto error = llvm::sys::fs::setPermissions(targetPath, *permissions)) {
        ABORT("couldn't set permissions for '" << targetPath << "': " << error.message());
    }
    if (auto error = llvm::sys::fs::remove(sourcePath)) {
        ABORT("couldn't remove '" << sourcePath << "': " << error.message());
    }
}

void delta::printDiagnostic(SourceLocation location, llvm::StringRef type, llvm::raw_ostream::Colors color, llvm::StringRef message) {
    if (llvm::outs().has_colors()) {
        llvm::outs().changeColor(llvm::raw_ostream::SAVEDCOLOR, true);
    }

    if (location.file && *location.file) {
        llvm::outs() << location.file;
        if (location.isValid()) {
            llvm::outs() << ':' << location.line << ':' << location.column;
        }
        llvm::outs() << ": ";
    }

    printColored(type, color);
    printColored(": ", color);
    printColored(message, llvm::raw_ostream::SAVEDCOLOR);

    if (location.file && *location.file && location.isValid()) {
        auto line = readLineFromFile(location);
        llvm::outs() << '\n' << line << '\n';

        for (char ch : line.substr(0, location.column - 1)) {
            llvm::outs() << (ch != '\t' ? ' ' : '\t');
        }
        printColored('^', llvm::raw_ostream::GREEN);
    }

    llvm::outs() << '\n';
}

std::string delta::getCCompilerPath() {
#ifdef _WIN32
    auto compilers = { "cl.exe", "clang-cl.exe" };
#else
    auto compilers = { "cc", "clang", "gcc" };
#endif
    for (const char* compiler : compilers) {
        if (auto path = llvm::sys::findProgramByName(compiler)) {
            return std::move(*path);
        }
    }
    return "";
}

void delta::abort(StringFormatter& message) {
    printColored("error: ", llvm::raw_ostream::RED);
    llvm::outs() << message.str() << '\n';
    exit(1);
}

void delta::fatalError(SourceLocation location, StringFormatter& message) {
    error(location, message);
    exit(1);
}

void delta::errorWithNotes(SourceLocation location, std::vector<Note>&& notes, StringFormatter& message) {
    printDiagnostic(location, "error", llvm::raw_ostream::RED, message.str());

    for (auto& note : notes) {
        printDiagnostic(note.location, "note", llvm::raw_ostream::BLACK, note.message);
    }
}

void delta::error(SourceLocation location, StringFormatter& message) {
    printDiagnostic(location, "error", llvm::raw_ostream::RED, message.str());
}

void delta::warn(SourceLocation location, StringFormatter& message) {
    extern llvm::cl::opt<WarningMode> warningMode;

    switch (warningMode) {
        case WarningMode::Default:
            printDiagnostic(location, "warning", llvm::raw_ostream::YELLOW, message.str());
            break;
        case WarningMode::Suppress:
            break;
        case WarningMode::TreatAsErrors:
            error(location, message);
            break;
    }
}
