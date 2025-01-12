/****************************************************************************
 * Copyright (C) 2012-2016 Woboq GmbH
 * Olivier Goffart <contact at woboq.com>
 * https://woboq.com/codebrowser.html
 *
 * This file is part of the Woboq Code Browser.
 *
 * Commercial License Usage:
 * Licensees holding valid commercial licenses provided by Woboq may use
 * this file in accordance with the terms contained in a written agreement
 * between the licensee and Woboq.
 * For further information see https://woboq.com/codebrowser.html
 *
 * Alternatively, this work may be used under a Creative Commons
 * Attribution-NonCommercial-ShareAlike 3.0 (CC-BY-NC-SA 3.0) License.
 * http://creativecommons.org/licenses/by-nc-sa/3.0/deed.en_US
 * This license does not allow you to use the code browser to assist the
 * development of your commercial software. If you intent to do so, consider
 * purchasing a commercial licence.
 ****************************************************************************/

#include "clang/AST/ASTContext.h"
#include "clang/Frontend/FrontendActions.h"
#include "clang/Tooling/JSONCompilationDatabase.h"
#include "clang/Tooling/Tooling.h"
#include "llvm/Support/CommandLine.h"

#include <clang/Frontend/CompilerInstance.h>
#include <llvm/ADT/StringSwitch.h>
#include <llvm/Support/Path.h>

#include "annotator.h"
#include "browserastvisitor.h"
#include "compat.h"
#include "filesystem.h"
#include "preprocessorcallback.h"
#include "projectmanager.h"
#include "stringbuilder.h"
#include <ctime>
#include <fstream>
#include <iostream>
#include <limits>
#include <stdexcept>

#include "embedded_includes.h"
#include "threadpool.h"
#include "logger.h"

namespace cl = llvm::cl;

cl::opt<std::string> BuildPath(
    "b", cl::value_desc("build_path"),
    cl::desc(
        "Build path containing compilation database (compile_commands.json) If this argument is "
        "not passed, the compilation arguments can be passed on the command line after '--'"),
    cl::Optional);

cl::list<std::string> SourcePaths(cl::Positional, cl::desc("<sources>* [-- <compile command>]"),
                                  cl::ZeroOrMore);

cl::opt<std::string> OutputPath("o", cl::value_desc("output path"),
                                cl::desc("Output directory where the generated files will be put"),
                                cl::Required);

cl::list<std::string> ProjectPaths(
    "p", cl::value_desc("<project>:<path>[:<revision>]"),
    cl::desc(
        "Project specification: The name of the project, the absolute path of the source code, and "
        "the revision separated by colons. Example: -p projectname:/path/to/source/code:0.3beta"),
    cl::ZeroOrMore);


cl::list<std::string> ExternalProjectPaths(
    "e", cl::value_desc("<project>:<path>:<url>"),
    cl::desc("Reference to an external project. Example: -e "
             "clang/include/clang:/opt/llvm/include/clang/:https://code.woboq.org/llvm"),
    cl::ZeroOrMore);

cl::opt<std::string>
    DataPath("d", cl::value_desc("data path"),
             cl::desc("Data url where all the javascript and css files are found. Can be absolute, "
                      "or relative to the output directory. Defaults to ../data"),
             cl::Optional);

cl::opt<bool>
    ProcessAllSources("a",
                      cl::desc("Process all files from the compile_commands.json. If this argument "
                               "is passed, the list of sources does not need to be passed"));

cl::extrahelp extra(

    R"(

EXAMPLES:

Simple generation without compile command or project (compile command specified inline)
  codebrowser_generator -o ~/public_html/code -d https://code.woboq.org/data $PWD -- -std=c++14 -I/opt/llvm/include

With a project
  codebrowser_generator -b $PWD/build -a -p codebrowser:$PWD -o ~/public_html/code
)");

#if 1
std::string locationToString(clang::SourceLocation loc, clang::SourceManager &sm)
{
    clang::PresumedLoc fixed = sm.getPresumedLoc(loc);
    if (!fixed.isValid())
        return "???";
    return (llvm::Twine(fixed.getFilename()) + ":" + llvm::Twine(fixed.getLine())).str();
}
#endif

enum class DatabaseType {
    InDatabase,
    NotInDatabase,
    ProcessFullDirectory
};

struct BrowserDiagnosticClient : clang::DiagnosticConsumer
{
    Annotator &annotator;
    BrowserDiagnosticClient(Annotator &fm)
        : annotator(fm)
    {
    }

    static bool isImmintrinDotH(const clang::PresumedLoc &loc)
    {
        return llvm::StringRef(loc.getFilename()).contains("immintrin.h");
    }

    virtual void HandleDiagnostic(clang::DiagnosticsEngine::Level DiagLevel,
                                  const clang::Diagnostic &Info) override
    {
        std::string clas;
        llvm::SmallString<1000> diag;
        Info.FormatDiagnostic(diag);

        switch (DiagLevel) {
        case clang::DiagnosticsEngine::Fatal:
            // ignore tons of errors in immintrin.h
            if (isImmintrinDotH(annotator.getSourceMgr().getPresumedLoc(Info.getLocation())))
                return;
            std::cerr << "FATAL ";
            LLVM_FALLTHROUGH;
        case clang::DiagnosticsEngine::Error:
            std::cerr << "Error: " << locationToString(Info.getLocation(), annotator.getSourceMgr())
                      << ": " << diag.c_str() << std::endl;
            clas = "error";
            break;
        case clang::DiagnosticsEngine::Warning:
            clas = "warning";
            break;
        default:
            return;
        }
        clang::SourceRange Range = Info.getLocation();
        annotator.reportDiagnostic(Range, diag.c_str(), clas);
    }
};

class BrowserASTConsumer : public clang::ASTConsumer
{
    clang::CompilerInstance &ci;
    Annotator annotator;
    DatabaseType WasInDatabase;

public:
    BrowserASTConsumer(clang::CompilerInstance &ci, ProjectManager &projectManager,
                       DatabaseType WasInDatabase)
        : clang::ASTConsumer()
        , ci(ci)
        , annotator(projectManager)
        , WasInDatabase(WasInDatabase)
    {
		SPDLOG_DEBUG("BrowserASTConsumer constructor");
        // ci.getLangOpts().DelayedTemplateParsing = (true);
#if CLANG_VERSION_MAJOR < 16
        // the meaning of this function has changed which causes
        // a lot of issues in clang 16
        ci.getPreprocessor().enableIncrementalProcessing();
#endif
    }
    virtual ~BrowserASTConsumer()
    {
		SPDLOG_DEBUG("BrowserASTConsumer destructor");
        ci.getDiagnostics().setClient(new clang::IgnoringDiagConsumer, true);
    }

    virtual void Initialize(clang::ASTContext &Ctx) override
    {
        annotator.setSourceMgr(Ctx.getSourceManager(), Ctx.getLangOpts());
        annotator.setMangleContext(Ctx.createMangleContext());
        ci.getPreprocessor().addPPCallbacks(maybe_unique(new PreprocessorCallback(
            annotator, ci.getPreprocessor(), WasInDatabase == DatabaseType::ProcessFullDirectory)));
        ci.getDiagnostics().setClient(new BrowserDiagnosticClient(annotator), true);
        ci.getDiagnostics().setErrorLimit(0);
    }

    virtual bool HandleTopLevelDecl(clang::DeclGroupRef D) override
    {
        if (ci.getDiagnostics().hasFatalErrorOccurred()) {
			SPDLOG_DEBUG("Reset errors: (Hack to ignore the fatal errors.)");
            // Reset errors: (Hack to ignore the fatal errors.)
            ci.getDiagnostics().Reset();
            // When there was fatal error, processing the warnings may cause crashes
            ci.getDiagnostics().setIgnoreAllWarnings(true);
        }
        return true;
    }

    virtual void HandleTranslationUnit(clang::ASTContext &Ctx) override
    {

        /* if (PP.getDiagnostics().hasErrorOccurred())
             return;*/
        ci.getPreprocessor().getDiagnostics().getClient();


        BrowserASTVisitor v(annotator);
		SPDLOG_DEBUG("Create BrowserASTVisitor");
        v.TraverseDecl(Ctx.getTranslationUnitDecl());
		SPDLOG_DEBUG("TraverseDecl done");


        annotator.generate(ci.getSema(), WasInDatabase != DatabaseType::NotInDatabase);
    }

    virtual bool shouldSkipFunctionBody(clang::Decl *D) override
    {
        return !annotator.shouldProcess(
            clang::FullSourceLoc(D->getLocation(), annotator.getSourceMgr())
                .getExpansionLoc()
                .getFileID());
    }
};

class ProcessedSet {
	public:
		bool try_insert(const std::string& s) {
			std::lock_guard lg(mutex_);
			auto [_,suc] = processed_.insert(s);
			return suc;
		}
		static ProcessedSet& get() {
			static ProcessedSet inst;
			return inst;
		}
	private:
		std::mutex mutex_;
    	std::set<std::string> processed_;
};

class BrowserAction : public clang::ASTFrontendAction
{
    //static std::set<std::string> processed;
    DatabaseType WasInDatabase;

protected:
#if CLANG_VERSION_MAJOR == 3 && CLANG_VERSION_MINOR <= 5
    virtual clang::ASTConsumer *
#else
    virtual std::unique_ptr<clang::ASTConsumer>
#endif
    CreateASTConsumer(clang::CompilerInstance &CI, llvm::StringRef InFile) override
    {
		SPDLOG_DEBUG("Start CreateASTConsumer for:{}", InFile.str());
        //if (processed.count(InFile.str())) {
        if(!ProcessedSet::get().try_insert(InFile.str())) {
			SPDLOG_ERROR("Skipping already processed:{}", InFile.str());
            std::cerr << "Skipping already processed " << InFile.str() << std::endl;
            return nullptr;
        }

        CI.getFrontendOpts().SkipFunctionBodies = true;

        return maybe_unique(new BrowserASTConsumer(CI, *projectManager, WasInDatabase));
    }

public:
    BrowserAction(DatabaseType WasInDatabase = DatabaseType::InDatabase)
        : WasInDatabase(WasInDatabase)
    {
		SPDLOG_DEBUG("BrowserAction constructor");
    }
    virtual bool hasCodeCompletionSupport() const override
    {
        return true;
    }
    static ProjectManager *projectManager;
};


//std::set<std::string> BrowserAction::processed;
ProjectManager *BrowserAction::projectManager = nullptr;

static bool proceedCommand(std::vector<std::string> command, llvm::StringRef Directory,
                           llvm::StringRef file,  DatabaseType WasInDatabase)
{
	SPDLOG_DEBUG("Start proceedCommand with: command: {}, Directory: {}, file:{}, was in db:{}", command, Directory.data(), file.data(), (int)WasInDatabase);
    llvm::IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> VFS(
        new llvm::vfs::OverlayFileSystem(llvm::vfs::getRealFileSystem()));
    clang::FileManager FM({ "." }, VFS);

    FM.Retain();
    // This code change all the paths to be absolute paths
    //  FIXME:  it is a bit fragile.
    bool previousIsDashI = false;
    bool previousNeedsMacro = false;
    bool hasNoStdInc = false;
    for (std::string &A : command) {
        if (previousIsDashI && !A.empty() && A[0] != '/') {
            A = Directory % "/" % A;
            previousIsDashI = false;
            continue;
        } else if (A == "-I") {
            previousIsDashI = true;
            continue;
        } else if (A == "-nostdinc" || A == "-nostdinc++") {
            hasNoStdInc = true;
            continue;
        } else if (A == "-U" || A == "-D") {
            previousNeedsMacro = true;
            continue;
        }
        if (previousNeedsMacro) {
            previousNeedsMacro = false;
            continue;
        }
        previousIsDashI = false;
        if (A.empty())
            continue;
        if (llvm::StringRef(A).startswith("-I") && A[2] != '/') {
            A = "-I" % Directory % "/" % llvm::StringRef(A).substr(2);
            continue;
        }
        if (A[0] == '-' || A[0] == '/')
            continue;
        std::string PossiblePath = Directory % "/" % A;
        if (llvm::sys::fs::exists(PossiblePath))
            A = PossiblePath;
    }

#if CLANG_VERSION_MAJOR == 3 && CLANG_VERSION_MINOR < 6
    auto Ajust = [&](clang::tooling::ArgumentsAdjuster &&aj) { command = aj.Adjust(command); };
    Ajust(clang::tooling::ClangSyntaxOnlyAdjuster());
    Ajust(clang::tooling::ClangStripOutputAdjuster());
#elif CLANG_VERSION_MAJOR == 3 && CLANG_VERSION_MINOR < 8
    command = clang::tooling::getClangSyntaxOnlyAdjuster()(command);
    command = clang::tooling::getClangStripOutputAdjuster()(command);
#else
    command = clang::tooling::getClangSyntaxOnlyAdjuster()(command, file);
    command = clang::tooling::getClangStripOutputAdjuster()(command, file);
#endif

    if (!hasNoStdInc) {
#ifndef _WIN32
        command.push_back("-isystem");
#else
        command.push_back("-I");
#endif

        command.push_back("/builtins");
    }

    command.push_back("-Qunused-arguments");
    command.push_back("-Wno-unknown-warning-option");
	SPDLOG_DEBUG("Start proceedCommand with adjusted: command: {}", command);
    clang::tooling::ToolInvocation Inv(command, maybe_unique(new BrowserAction(WasInDatabase)), &FM);

#if CLANG_VERSION_MAJOR <= 10
    if (!hasNoStdInc) {
    	SPDLOG_DEBUG("hasNoStdInc, Map the builtins includes");
        // Map the builtins includes
        const EmbeddedFile *f = EmbeddedFiles;
        while (f->filename) {
            Inv.mapVirtualFile(f->filename, { f->content, f->size });
            f++;
        }
    }
#endif

    bool result = Inv.run();
    if (!result) {
		SPDLOG_ERROR("Error: The file was not recognized as source code: : {}", file.str());
        std::cerr << "Error: The file was not recognized as source code: " << file.str()
                  << std::endl;
    }
    return result;
}
#include "spdlog/sinks/basic_file_sink.h"

int main(int argc, const char **argv)
{
	auto file_logger = spdlog::basic_logger_mt("codebrowser", "/tmp/codebrowserlog.txt");
	file_logger->flush_on(spdlog::level::trace);
	spdlog::set_default_logger(file_logger);
	spdlog::set_pattern("%T[%t][file: %s][fun: %!][line: %#] %v");


	spdlog::set_level(spdlog::level::trace);
	//spdlog::flush_every(std::chrono::seconds(1));
	SPDLOG_INFO("Start");
    std::string ErrorMessage;
    std::unique_ptr<clang::tooling::CompilationDatabase> Compilations(
        clang::tooling::FixedCompilationDatabase::loadFromCommandLine(argc, argv
#if CLANG_VERSION_MAJOR >= 5
                                                                      ,
                                                                      ErrorMessage
#endif
                                                                      ));
    if (!ErrorMessage.empty()) {
        std::cerr << ErrorMessage << std::endl;
        ErrorMessage = {};
    }

    llvm::cl::ParseCommandLineOptions(argc, argv);

#ifdef _WIN32
    make_forward_slashes(OutputPath._Get_data()._Myptr());
#endif

    ProjectManager projectManager(OutputPath, DataPath);
	ThreadPool thread_pool;
    for (std::string &s : ProjectPaths) {
    	SPDLOG_DEBUG("Try one project path:{}", s);
        auto colonPos = s.find(':');
        if (colonPos >= s.size()) {
            std::cerr << "fail to parse project option : " << s << std::endl;
            continue;
        }
        auto secondColonPos = s.find(':', colonPos + 1);
        ProjectInfo info { s.substr(0, colonPos),
                           s.substr(colonPos + 1, secondColonPos - colonPos - 1),
                           secondColonPos < s.size() ? s.substr(secondColonPos + 1)
                                                     : std::string() };
        if (!projectManager.addProject(std::move(info))) {
            std::cerr << "invalid project directory for : " << s << std::endl;
        }
    }
    for (std::string &s : ExternalProjectPaths) {
    	SPDLOG_DEBUG("Try one external project path:{}", s);
        auto colonPos = s.find(':');
        if (colonPos >= s.size()) {
            std::cerr << "fail to parse project option : " << s << std::endl;
            continue;
        }
        auto secondColonPos = s.find(':', colonPos + 1);
        if (secondColonPos >= s.size()) {
            std::cerr << "fail to parse project option : " << s << std::endl;
            continue;
        }
        ProjectInfo info { s.substr(0, colonPos),
                           s.substr(colonPos + 1, secondColonPos - colonPos - 1),
                           ProjectInfo::External };
        info.external_root_url = s.substr(secondColonPos + 1);
        if (!projectManager.addProject(std::move(info))) {
            std::cerr << "invalid project directory for : " << s << std::endl;
        }
    }
    BrowserAction::projectManager = &projectManager;


    if (!Compilations && llvm::sys::fs::exists(BuildPath)) {
    	SPDLOG_DEBUG("!Compilations && llvm::sys::fs::exists(BuildPath):{}", BuildPath);
        if (llvm::sys::fs::is_directory(BuildPath)) {
			SPDLOG_DEBUG("Build path is directory:{}, add to compilation database", BuildPath);
            Compilations = std::unique_ptr<clang::tooling::CompilationDatabase>(
                clang::tooling::CompilationDatabase::loadFromDirectory(BuildPath, ErrorMessage));
        } else {
			SPDLOG_DEBUG("Build path is not directory:{}, load from file", BuildPath);
            Compilations = std::unique_ptr<clang::tooling::CompilationDatabase>(
                clang::tooling::JSONCompilationDatabase::loadFromFile(
                    BuildPath, ErrorMessage
#if CLANG_VERSION_MAJOR >= 4
                    ,
                    clang::tooling::JSONCommandLineSyntax::AutoDetect
#endif
                    ));
        }
        if (!Compilations && !ErrorMessage.empty()) {
            std::cerr << ErrorMessage << std::endl;
        }
    }

    if (!Compilations) {
		SPDLOG_ERROR("Could not load compilationdatabase, exit");
        std::cerr
            << "Could not load compilationdatabase. "
               "Please use the -b option to a path containing a compile_commands.json, or use "
               "'--' followed by the compilation commands."
            << std::endl;
        return EXIT_FAILURE;
    }

    bool IsProcessingAllDirectory = false;
    std::vector<std::string> DirContents;
    std::vector<std::string> AllFiles = Compilations->getAllFiles();
    std::sort(AllFiles.begin(), AllFiles.end());
    llvm::ArrayRef<std::string> Sources = SourcePaths;
    if (Sources.empty() && ProcessAllSources) {
		SPDLOG_ERROR("Will process all files");
        // Because else the order is too random
        Sources = AllFiles;
    } else if (ProcessAllSources) {
        std::cerr << "Cannot use both sources and  '-a'" << std::endl;
        return EXIT_FAILURE;
    } else if (Sources.size() == 1 && llvm::sys::fs::is_directory(Sources.front())) {
		SPDLOG_DEBUG("Iterator through the directory: {}", Sources.front());
#if CLANG_VERSION_MAJOR != 3 || CLANG_VERSION_MINOR >= 5
        // A directory was passed, process all the files in that directory
        llvm::SmallString<128> DirName;
        llvm::sys::path::native(Sources.front(), DirName);
        while (DirName.endswith("/"))
            DirName.pop_back();
        std::error_code EC;
        for (llvm::sys::fs::recursive_directory_iterator it(DirName.str(), EC), DirEnd;
             it != DirEnd && !EC; it.increment(EC)) {
            if (llvm::sys::path::filename(it->path()).startswith(".")) {
                it.no_push();
                continue;
            }
            DirContents.push_back(it->path());
        }
        Sources = DirContents;
        IsProcessingAllDirectory = true;
        if (EC) {
            std::cerr << "Error reading the directory: " << EC.message() << std::endl;
            return EXIT_FAILURE;
        }

        if (ProjectPaths.empty()) {
            ProjectInfo info { std::string(llvm::sys::path::filename(DirName)),
                               std::string(DirName.str()) };
            projectManager.addProject(std::move(info));
        }
#else
        std::cerr << "Passing directory is only implemented with llvm >= 3.5" << std::endl;
        return EXIT_FAILURE;
#endif
    }

    if (Sources.empty()) {
        std::cerr << "No source files.  Please pass source files as argument, or use '-a'"
                  << std::endl;
        return EXIT_FAILURE;
    }
    if (ProjectPaths.empty() && !IsProcessingAllDirectory) {
        std::cerr << "You must specify a project name and directory with '-p name:directory'"
                  << std::endl;
        return EXIT_FAILURE;
    }

#if CLANG_VERSION_MAJOR >= 12
    llvm::IntrusiveRefCntPtr<llvm::vfs::OverlayFileSystem> VFS(
        new llvm::vfs::OverlayFileSystem(llvm::vfs::getRealFileSystem()));
    clang::FileManager FM({ "." }, VFS);
#else
    clang::FileManager FM({ "." });
#endif
    FM.Retain();

#if CLANG_VERSION_MAJOR >= 12
    // Map virtual files
    {
        auto InMemoryFS = llvm::IntrusiveRefCntPtr<llvm::vfs::InMemoryFileSystem>(
            new llvm::vfs::InMemoryFileSystem);
        VFS->pushOverlay(InMemoryFS);
        // Map the builtins includes
        const EmbeddedFile *f = EmbeddedFiles;
        while (f->filename) {
            InMemoryFS->addFile(f->filename, 0, llvm::MemoryBuffer::getMemBufferCopy(f->content));
            f++;
        }
    }
#endif

    int Progress = 0;

    std::vector<std::string> NotInDB;

    for (const auto &it : Sources) {
		SPDLOG_DEBUG("Prepare work for source: {}", it);
        std::string file = clang::tooling::getAbsolutePath(it);
		SPDLOG_DEBUG("Absolute file path: {}", file);
        Progress++;

        if (it.empty() || it == "-")
            continue;

        llvm::SmallString<256> filename;
        canonicalize(file, filename);

        if (auto project = projectManager.projectForFile(filename)) {
			SPDLOG_DEBUG("The project for file: {}, {}", filename.c_str(), project->name);
            if (!projectManager.shouldProcess0(filename, project)) {
				SPDLOG_ERROR("Sources: Skipping already processed : {}", filename.c_str());
                std::cerr << "Sources: Skipping already processed " << filename.c_str()
                          << std::endl;
                continue;
            }
        } else {
			SPDLOG_ERROR("Sources: Skipping file not included by any project : {}", filename.c_str());
            std::cerr << "Sources: Skipping file not included by any project " << filename.c_str()
                      << std::endl;
            continue;
        }

        bool isHeader = llvm::StringSwitch<bool>(llvm::sys::path::extension(filename))
                            .Cases(".h", ".H", ".hh", ".hpp", true)
                            .Default(false);

		SPDLOG_DEBUG("File is header: {}, {}", filename.c_str(), isHeader);
        auto compileCommandsForFile = Compilations->getCompileCommands(file);
        if (!compileCommandsForFile.empty() && !isHeader) {
			SPDLOG_DEBUG("compileCommandsForFile: {}", compileCommandsForFile.front().CommandLine);
            std::cerr << '[' << (100 * Progress / Sources.size()) << "%] Processing " << file
                      << "\n";
            auto command = compileCommandsForFile.front().CommandLine;
            auto dir = compileCommandsForFile.front().Directory;
            auto tp = IsProcessingAllDirectory ? DatabaseType::ProcessFullDirectory
                                                    : DatabaseType::InDatabase;
			thread_pool.Schedule([command = std::move(command), dir = std::move(dir), file=std::move(file), tp=tp](){
					proceedCommand(std::move(command), dir, file, tp);
                    		});

        } else {
			SPDLOG_DEBUG("Add delayed file to queue: {}", filename.c_str());
            // TODO: Try to find a command line for a file in the same path
            std::cerr << "Delayed " << file << "\n";
            Progress--;
            NotInDB.push_back(std::string(filename.str()));
            continue;
        }
    }

	SPDLOG_DEBUG("Delayed queue: {}", NotInDB);
    for (const auto &it : NotInDB) {
		SPDLOG_DEBUG("Start to process delay file from queue: {}", it);
        std::string file = clang::tooling::getAbsolutePath(it);
		SPDLOG_DEBUG("Absolute file path: {}", file);
        Progress++;

        if (auto project = projectManager.projectForFile(file)) {
			SPDLOG_DEBUG("The project for file: {}, {}", file.c_str(), project->name);
            if (!projectManager.shouldProcess(file, project)) {
				SPDLOG_ERROR("NotInDB: Skipping already processed : {}", file.c_str());
                std::cerr << "NotInDB: Skipping already processed " << file.c_str() << std::endl;
                continue;
            }
        } else {
			SPDLOG_ERROR("NotInDB: Skipping file not included by any project", file.c_str());
            std::cerr << "NotInDB: Skipping file not included by any project " << file.c_str()
                      << std::endl;
            continue;
        }

        llvm::StringRef similar;

        auto compileCommandsForFile = Compilations->getCompileCommands(file);
        std::string fileForCommands = file;
        if (compileCommandsForFile.empty()) {
			SPDLOG_ERROR("NotInDB: compileCommandsForFile is empty:{}", file.c_str());
 
            // Find the element with the bigger prefix
            auto lower = std::lower_bound(AllFiles.cbegin(), AllFiles.cend(), file);
            if (lower == AllFiles.cend())
                lower = AllFiles.cbegin();
            compileCommandsForFile = Compilations->getCompileCommands(*lower);
            fileForCommands = *lower;
        }

        bool success = false;
        if (!compileCommandsForFile.empty()) {
            std::cerr << '[' << (100 * Progress / Sources.size()) << "%] Processing " << file
                      << "\n";
            auto command = compileCommandsForFile.front().CommandLine;
			SPDLOG_ERROR("NotInDB: final compileCommandsForFile: {}", command);
            std::replace(command.begin(), command.end(), fileForCommands, it);
			SPDLOG_ERROR("NotInDB: final after replace compileCommandsForFile: {}", command);
            if (llvm::StringRef(file).endswith(".qdoc")) {
                command.insert(command.begin() + 1, "-xc++");
                // include the header for this .qdoc file
                command.push_back("-include");
                command.push_back(llvm::StringRef(file).substr(0, file.size() - 5) % ".h");
            }
			
			SPDLOG_ERROR("NotInDB: final after pushbacks compileCommandsForFile", command);

			auto dir = compileCommandsForFile.front().Directory;
			auto tp = IsProcessingAllDirectory ? DatabaseType::ProcessFullDirectory
                                                              : DatabaseType::NotInDatabase;
            thread_pool.Schedule([command = std::move(command), dir = std::move(dir), file=std::move(file), tp=tp](){proceedCommand(std::move(command), dir,
                                     file, tp);
                    });
        } else {
            std::cerr << "Could not find commands for " << file << "\n";
        }

		SPDLOG_DEBUG("Normal process done");
        if (!success && !IsProcessingAllDirectory) {
            std::cerr << "Run into !success && !IsProcessingAllDirectory" << "\n";
            ProjectInfo *projectinfo = projectManager.projectForFile(file);
            if (!projectinfo)
                continue;
            if (!projectManager.shouldProcess(file, projectinfo))
                continue;

            auto now = std::time(0);
            auto tm = localtime(&now);
            char buf[80];
            std::strftime(buf, sizeof(buf), "%Y-%b-%d", tm);

            std::string footer = "Generated on <em>" % std::string(buf) % "</em>" % " from project "
                % projectinfo->name % "</a>";
            if (!projectinfo->revision.empty())
                footer %= " revision <em>" % projectinfo->revision % "</em>";

#if CLANG_VERSION_MAJOR == 3 && CLANG_VERSION_MINOR <= 4
            llvm::OwningPtr<llvm::MemoryBuffer> Buf;
            if (!llvm::MemoryBuffer::getFile(file, Buf))
                continue;
#else
            auto B = llvm::MemoryBuffer::getFile(file);
            if (!B)
                continue;
            std::unique_ptr<llvm::MemoryBuffer> Buf = std::move(B.get());
#endif

            std::string fn = projectinfo->name % "/"
                % llvm::StringRef(file).substr(projectinfo->source_path.size());

            Generator g;
            g.generate(projectManager.outputPrefix, projectManager.dataPath, fn,
                       Buf->getBufferStart(), Buf->getBufferEnd(), footer,
                       "Warning: This file is not a C or C++ file. It does not have highlighting.",
                       std::set<std::string>());

            std::ofstream fileIndex;
            fileIndex.open(projectManager.outputPrefix + "/otherIndex", std::ios::app);
            if (!fileIndex)
                continue;
            fileIndex << fn << '\n';
        }
    }
	SPDLOG_DEBUG("All process done");
}
