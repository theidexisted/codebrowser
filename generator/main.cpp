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
#include <latch>

#include <clang/Basic/LangOptions.h>
#include <clang/Basic/SourceManager.h>
#include <clang/Basic/TargetInfo.h>
#include <clang/Driver/Action.h>
#include <clang/Driver/Compilation.h>
#include <clang/Driver/Driver.h>
#include <clang/Driver/Tool.h>
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Lex/PreprocessorOptions.h>
#include <llvm/ADT/StringSwitch.h>
#include <llvm/Support/Path.h>
#include <llvm/TargetParser/Host.h>

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

#include "logger.h"
#include "threadpool.h"
// #include "spdlog/sinks/basic_file_sink.h"
#include "spdlog/sinks/rotating_file_sink.h"

extern "C" void __tsan_on_report()
{
    // This function will be called whenever a data race is reported.
    // Place a breakpoint here.
    __builtin_trap(); // Trigger a breakpoint in gdb.
}

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

cl::opt<bool> IsDebug("dbg", cl::desc("If set debug log on"), cl::desc("More debug logs"),
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

This version is patched with multiple threads.
)");



std::string locationToString(clang::SourceLocation loc, clang::SourceManager &sm)
{
    clang::PresumedLoc fixed = sm.getPresumedLoc(loc);
    if (!fixed.isValid())
        return "???";
    return (llvm::Twine(fixed.getFilename()) + ":" + llvm::Twine(fixed.getLine())).str();
}

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
        // the meaning of this function has changed which causes
        // a lot of issues in clang 16
        ci.getPreprocessor().enableIncrementalProcessing();
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

class ProcessedSet
{
public:
    bool try_insert(const std::string &s)
    {
        std::lock_guard lg(mutex_);
        auto [_, suc] = processed_.insert(s);
        return suc;
    }
    static ProcessedSet &get()
    {
        static ProcessedSet inst;
        return inst;
    }

private:
    std::mutex mutex_;
    std::set<std::string> processed_;
};

class BrowserAction : public clang::ASTFrontendAction
{
    // static std::set<std::string> processed;
    DatabaseType WasInDatabase;

protected:
    virtual std::unique_ptr<clang::ASTConsumer>
    CreateASTConsumer(clang::CompilerInstance &CI, llvm::StringRef InFile) override
    {
        SPDLOG_DEBUG("Start CreateASTConsumer for:{}", InFile.str());
        llvm::SmallString<256> canonicalInput;
        canonicalize(InFile, canonicalInput);
        llvm::StringRef dedupKey =
            canonicalInput.empty() ? InFile : llvm::StringRef(canonicalInput);

        if (!ProcessedSet::get().try_insert(dedupKey.str())) {
            SPDLOG_ERROR("Skipping already processed:{}", dedupKey.str());
            std::cerr << "Skipping already processed " << dedupKey.str() << std::endl;
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


ProjectManager *BrowserAction::projectManager = nullptr;

using namespace clang;
std::unique_ptr<CompilerInvocation>
buildCompilerInvocation(const std::string &main, std::vector<const char *> args,
                        IntrusiveRefCntPtr<llvm::vfs::FileSystem> vfs)
{
    // std::string save = "-resource-dir=" /*+ g_config->clang.resourceDir*/;
    // args.push_back(save.c_str());
    args.push_back("-fsyntax-only");

    // Similar to clang/tools/driver/driver.cpp:insertTargetAndModeArgs but don't
    // require llvm::InitializeAllTargetInfos().
    auto target_and_mode = driver::ToolChain::getTargetAndModeFromProgramName(args[0]);
    if (target_and_mode.DriverMode)
        args.insert(args.begin() + 1, target_and_mode.DriverMode);
    if (!target_and_mode.TargetPrefix.empty()) {
        const char *arr[] = { "-target", target_and_mode.TargetPrefix.c_str() };
        args.insert(args.begin() + 1, std::begin(arr), std::end(arr));
    }

    args.push_back("-fms-extensions");

    DiagnosticOptions diagOpts;
    IntrusiveRefCntPtr<DiagnosticsEngine> diags(
        CompilerInstance::createDiagnostics(*vfs, diagOpts, new IgnoringDiagConsumer, true));
    driver::Driver d(args[0], llvm::sys::getDefaultTargetTriple(), *diags, "ccls", vfs);
    d.setCheckInputsExist(false);
    // For -include b.hh, don't probe b.hh.{gch,pch} and change to -include-pch.
    d.setProbePrecompiled(false);
    static std::mutex mut_for_driver;
    std::unique_ptr<driver::Compilation> comp;
    {
        std::lock_guard lg(mut_for_driver);
        comp.reset(d.BuildCompilation(args));
    }
    if (!comp)
        return nullptr;
    const driver::JobList &jobs = comp->getJobs();
    bool offload_compilation = false;
    if (jobs.size() > 1) {
        for (auto &a : comp->getActions()) {
            // On MacOSX real actions may end up being wrapped in BindArchAction
            if (isa<driver::BindArchAction>(a))
                a = *a->input_begin();
            if (isa<driver::OffloadAction>(a)) {
                offload_compilation = true;
                break;
            }
        }
        if (!offload_compilation)
            return nullptr;
    }
    if (jobs.size() == 0 || !isa<driver::Command>(*jobs.begin()))
        return nullptr;

    const driver::Command &cmd = cast<driver::Command>(*jobs.begin());
    if (StringRef(cmd.getCreator().getName()) != "clang")
        return nullptr;
    const llvm::opt::ArgStringList &cc_args = cmd.getArguments();
    auto ci = std::make_unique<CompilerInvocation>();
    if (!CompilerInvocation::CreateFromArgs(*ci, cc_args, *diags))
        return nullptr;

    ci->getDiagnosticOpts().IgnoreWarnings = true;
    ci->getFrontendOpts().DisableFree = false;
    // Enable IndexFrontendAction::shouldSkipFunctionBody.
    ci->getFrontendOpts().SkipFunctionBodies = true;
    ci->getLangOpts().SpellChecking = false;
    ci->getLangOpts().RecoveryAST = true;
    ci->getLangOpts().RecoveryASTType = true;
    auto &isec = ci->getFrontendOpts().Inputs;
    if (isec.size())
        isec[0] = FrontendInputFile(main, isec[0].getKind(), isec[0].isSystem());
    ci->getPreprocessorOpts().DisablePragmaDebugCrash = true;
    // clangSerialization has an unstable format. Disable PCH reading/writing
    // to work around PCH mismatch problems.
    ci->getPreprocessorOpts().ImplicitPCHInclude.clear();
    ci->getPreprocessorOpts().PrecompiledPreambleBytes = { 0, false };
    ci->getPreprocessorOpts().PCHThroughHeader.clear();

    ci->getHeaderSearchOpts().ModuleFormat = "raw";
    return ci;
}

class IndexDiags : public DiagnosticConsumer
{
public:
    llvm::SmallString<64> message;
    void HandleDiagnostic(DiagnosticsEngine::Level level, const clang::Diagnostic &info) override
    {
        DiagnosticConsumer::HandleDiagnostic(level, info);
        if (message.empty())
            info.FormatDiagnostic(message);
    }
};

static std::string buildHtmlFooter(const ProjectInfo &projectinfo)
{
    auto now = std::time(nullptr);
    auto tm = localtime(&now);
    char buf[80];
    std::strftime(buf, sizeof(buf), "%Y-%b-%d", tm);

    std::string footer =
        "Generated on <em>" % std::string(buf) % "</em>" % " from project " % projectinfo.name;
    if (!projectinfo.revision.empty())
        footer %= " revision <em>" % projectinfo.revision % "</em>";
    return footer;
}

static bool generatePlainFile(ProjectManager &projectManager, const std::string &file)
{
    ProjectInfo *projectinfo = projectManager.projectForFile(file);
    if (!projectinfo || !projectManager.shouldProcess(file, projectinfo))
        return false;

    auto B = llvm::MemoryBuffer::getFile(file);
    if (!B)
        return false;
    std::unique_ptr<llvm::MemoryBuffer> Buf = std::move(B.get());

    std::string fn =
        projectinfo->name % "/" % llvm::StringRef(file).substr(projectinfo->source_path.size());

    Generator g;
    g.generate(projectManager.outputPrefix, projectManager.dataPath, fn, Buf->getBufferStart(),
               Buf->getBufferEnd(), buildHtmlFooter(*projectinfo),
               "Warning: This file is not a C or C++ file. It does not have highlighting.",
               std::set<std::string>());

    std::ofstream fileIndex(projectManager.outputPrefix + "/otherIndex", std::ios::app);
    if (!fileIndex)
        return false;
    fileIndex << fn << '\n';
    return true;
}

static bool buildDelayedCommand(const clang::tooling::CompilationDatabase &compilations,
                                const std::vector<std::string> &allFiles,
                                const std::string &file, std::vector<std::string> &command,
                                std::string &directory)
{
    auto compileCommandsForFile = compilations.getCompileCommands(file);
    std::string fileForCommands = file;
    if (compileCommandsForFile.empty()) {
        auto lower = std::lower_bound(allFiles.cbegin(), allFiles.cend(), file);
        if (lower == allFiles.cend())
            lower = allFiles.cbegin();
        if (lower == allFiles.cend())
            return false;
        compileCommandsForFile = compilations.getCompileCommands(*lower);
        fileForCommands = *lower;
    }

    if (compileCommandsForFile.empty())
        return false;

    command = compileCommandsForFile.front().CommandLine;
    std::replace(command.begin(), command.end(), fileForCommands, file);
    if (llvm::StringRef(file).ends_with(".qdoc")) {
        command.insert(command.begin() + 1, "-xc++");
        command.push_back("-include");
        command.push_back(llvm::StringRef(file).substr(0, file.size() - 5) % ".h");
    }
    directory = compileCommandsForFile.front().Directory;
    return true;
}



bool index(const std::string &main, const std::vector<const char *> &args,
           // const std::vector<std::pair<std::string, std::string>> &remapped,
           DatabaseType WasInDatabase)
{

    auto pch = std::make_shared<PCHContainerOperations>();
    llvm::IntrusiveRefCntPtr<llvm::vfs::FileSystem> fs = llvm::vfs::getRealFileSystem();
    std::shared_ptr<CompilerInvocation> ci = buildCompilerInvocation(main, args, fs);
    // e.g. .s
    if (!ci)
        return false;
        // -fparse-all-comments enables documentation in the indexer and in
        // code completion.
    ci->getLangOpts().CommentOpts.ParseAllComments = true;
    ci->getLangOpts().RetainCommentsFromSystemHeaders = true;
    /*
      std::string buf = wfiles->getContent(main);
      std::vector<std::unique_ptr<llvm::MemoryBuffer>> bufs;
      if (buf.size())
        for (auto &[filename, content] : remapped) {
          bufs.push_back(llvm::MemoryBuffer::getMemBuffer(content));
          ci->getPreprocessorOpts().addRemappedFile(filename, bufs.back().get());
        }
    */
    IndexDiags dc;
    auto clang = std::make_unique<CompilerInstance>(ci, pch);
    clang->createDiagnostics(&dc, false);
    clang->getDiagnostics().setIgnoreAllWarnings(true);
    clang->setTarget(TargetInfo::CreateTargetInfo(clang->getDiagnostics(),
                                                  clang->getInvocation().getTargetOpts()));
    if (!clang->hasTarget())
        return {};
    clang->getPreprocessorOpts().RetainRemappedFileBuffers = true;
    clang->setVirtualFileSystem(fs);
    clang->createFileManager();
    clang->createSourceManager();

/*
  IndexParam param(*vfs, no_linkage);

  index::IndexingOptions indexOpts;
  indexOpts.SystemSymbolFilter =
      index::IndexingOptions::SystemSymbolFilterKind::All;
  if (no_linkage) {
    indexOpts.IndexFunctionLocals = true;
    indexOpts.IndexImplicitInstantiation = true;
#if LLVM_VERSION_MAJOR >= 9

    indexOpts.IndexParametersInDeclarations =
        g_config->index.parametersInDeclarations;
    indexOpts.IndexTemplateParameters = true;
#endif
  }
*/
    auto action = std::make_unique<BrowserAction>(WasInDatabase);

    std::string reason;
    if (!action->BeginSourceFile(*clang, clang->getFrontendOpts().Inputs[0]))
        return false;
    if (llvm::Error e = action->Execute()) {
        reason = llvm::toString(std::move(e));
        action->EndSourceFile();
        SPDLOG_ERROR("failed to index {}{}", main, reason.empty() ? "" : ": " + reason);
        return false;
    }
    action->EndSourceFile();
    if (!reason.empty()) {
        SPDLOG_ERROR("failed to index {}{}", main, reason.empty() ? "" : ": " + reason);
        return false;
    }
    SPDLOG_INFO("clang index finished for {}", main);
    return true;
}
std::string getClangResourceDir()
{
    return CLANG_RESOURCE_DIRECTORY;
}

static bool proceedCommand(std::vector<std::string> command, llvm::StringRef Directory,
                           llvm::StringRef file, DatabaseType WasInDatabase)
{
    SPDLOG_DEBUG("Start proceedCommandccls with: command: {}, Directory: {}, file:{}, was in db:{}",
                 command, Directory.data(), file.data(), ( int )WasInDatabase);
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
        if (llvm::StringRef(A).starts_with("-I") && A[2] != '/') {
            A = "-I" % Directory % "/" % llvm::StringRef(A).substr(2);
            continue;
        }
        if (A[0] == '-' || A[0] == '/')
            continue;
        std::string PossiblePath = Directory % "/" % A;
        if (llvm::sys::fs::exists(PossiblePath))
            A = PossiblePath;
    }

    command = clang::tooling::getClangSyntaxOnlyAdjuster()(command, file);
    command = clang::tooling::getClangStripOutputAdjuster()(command, file);

    if (!hasNoStdInc) {
#ifndef _WIN32
        command.push_back("-isystem");
#else
        command.push_back("-I");
#endif

        command.push_back("/builtins");
    }
    command.push_back("-resource-dir");
    command.push_back(getClangResourceDir());

    command.push_back("-Qunused-arguments");
    command.push_back("-Wno-unknown-warning-option");
    SPDLOG_DEBUG("Start proceedCommand with adjusted: command: {}", command);

    std::vector<const char *> cmd;
    for (const auto &s : command)
        cmd.emplace_back(s.data());
    auto result = index(file.data(), cmd, WasInDatabase);
    return result;
}



int main(int argc, const char **argv)
{
    // auto file_logger = spdlog::basic_logger_mt("codebrowser", "/tmp/codebrowserlog.txt");
    auto file_logger = spdlog::rotating_logger_mt("file_logger", "/tmp/codebrowserlog.txt",
                                                  1024 * 1024 * 50, 3, true);
    file_logger->flush_on(spdlog::level::trace);
    spdlog::set_default_logger(file_logger);
    spdlog::set_pattern("%T[%L][%t][file: %s][fun: %!][line: %#] %v");
    // spdlog::set_level(spdlog::level::err);
    // spdlog::flush_every(std::chrono::seconds(1));
    SPDLOG_INFO("Start");
    std::string ErrorMessage;
    std::unique_ptr<clang::tooling::CompilationDatabase> Compilations(
        clang::tooling::FixedCompilationDatabase::loadFromCommandLine(argc, argv, ErrorMessage));
    if (!ErrorMessage.empty()) {
        std::cerr << ErrorMessage << std::endl;
        ErrorMessage = {};
    }

    llvm::cl::ParseCommandLineOptions(argc, argv);


    if (IsDebug) {
        file_logger->set_level(spdlog::level::debug);
        SPDLOG_INFO("Set debug log on");
    }



#ifdef _WIN32
    make_forward_slashes(OutputPath._Get_data()._Myptr());
#endif

    size_t num_threads = std::max<size_t>(1, std::thread::hardware_concurrency());
    ThreadPool thread_pool(num_threads);
    ProjectManager projectManager(OutputPath, DataPath);
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
                    BuildPath, ErrorMessage, clang::tooling::JSONCommandLineSyntax::AutoDetect));
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
        SPDLOG_INFO("Will process all files");
        // Because else the order is too random
        Sources = AllFiles;
    } else if (ProcessAllSources) {
        std::cerr << "Cannot use both sources and  '-a'" << std::endl;
        return EXIT_FAILURE;
    } else if (Sources.size() == 1 && llvm::sys::fs::is_directory(Sources.front())) {
        SPDLOG_DEBUG("Iterator through the directory: {}", Sources.front());
        // A directory was passed, process all the files in that directory
        llvm::SmallString<128> DirName;
        llvm::sys::path::native(Sources.front(), DirName);
        while (DirName.ends_with("/"))
            DirName.pop_back();
        std::error_code EC;
        for (llvm::sys::fs::recursive_directory_iterator it(DirName.str(), EC), DirEnd;
             it != DirEnd && !EC; it.increment(EC)) {
            if (llvm::sys::path::filename(it->path()).starts_with(".")) {
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

    std::vector<std::string> NotInDB;

    std::latch completion_latch(Sources.size());
    for (const auto &it : Sources) {
        SPDLOG_DEBUG("Prepare work for source: {}", it);
        std::string file = clang::tooling::getAbsolutePath(it);
        SPDLOG_DEBUG("Absolute file path: {}", file);

        if (it.empty() || it == "-") {
            completion_latch.count_down();
            continue;
        }

        llvm::SmallString<256> filename;
        canonicalize(file, filename);

        if (auto project = projectManager.projectForFile(filename)) {
            SPDLOG_DEBUG("The project for file: {}, {}", filename.c_str(), project->name);
            if (!projectManager.shouldProcess0(filename, project)) {
                SPDLOG_ERROR("Sources: Skipping already processed : {}", filename.c_str());
                std::cerr << "Sources: Skipping already processed " << filename.c_str()
                          << std::endl;
                completion_latch.count_down();
                continue;
            }
        } else {
            SPDLOG_ERROR("Sources: Skipping file not included by any project : {}",
                         filename.c_str());
            std::cerr << "Sources: Skipping file not included by any project " << filename.c_str()
                      << std::endl;
            completion_latch.count_down();
            continue;
        }

        bool isHeader = llvm::StringSwitch<bool>(llvm::sys::path::extension(filename))
                            .Cases({ ".h", ".H", ".hh", ".hpp" }, true)
                            .Default(false);

        SPDLOG_DEBUG("File is header: {}, {}", filename.c_str(), isHeader);
        auto compileCommandsForFile = Compilations->getCompileCommands(file);
        if (!compileCommandsForFile.empty() && !isHeader) {
            SPDLOG_DEBUG("compileCommandsForFile: {}", compileCommandsForFile.front().CommandLine);
            // std::cerr << '[' << (100 * Progress / Sources.size()) << "%] Processing " << file
            //           << "\n";
            auto command = compileCommandsForFile.front().CommandLine;
            auto dir = compileCommandsForFile.front().Directory;
            auto tp = IsProcessingAllDirectory ? DatabaseType::ProcessFullDirectory
                                               : DatabaseType::InDatabase;
            thread_pool.Schedule([command = std::move(command), dir = std::move(dir),
                                  file = std::move(file), tp = tp, &completion_latch]() {
                proceedCommand(std::move(command), dir, file, tp);
                completion_latch.count_down();
            });

        } else {
            SPDLOG_DEBUG("Add delayed file to queue: {}", filename.c_str());
            // TODO: Try to find a command line for a file in the same path
            std::cerr << "Delayed " << file << "\n";
            NotInDB.push_back(std::string(filename.str()));
            continue;
        }
    }

    SPDLOG_DEBUG("Delayed queue: {}", NotInDB);
    for (const auto &it : NotInDB) {
        SPDLOG_DEBUG("Start to process delay file from queue: {}", it);
        std::string file = clang::tooling::getAbsolutePath(it);
        SPDLOG_DEBUG("Absolute file path: {}", file);

        auto project = projectManager.projectForFile(file);
        if (!project) {
            SPDLOG_ERROR("NotInDB: Skipping file not included by any project: {}", file.c_str());
            std::cerr << "NotInDB: Skipping file not included by any project " << file.c_str()
                      << std::endl;
            completion_latch.count_down();
            continue;
        }

        std::vector<std::string> command;
        std::string dir;
        if (buildDelayedCommand(*Compilations, AllFiles, file, command, dir)) {
            auto tp = IsProcessingAllDirectory ? DatabaseType::ProcessFullDirectory
                                               : DatabaseType::NotInDatabase;
            thread_pool.Schedule([command = std::move(command), dir = std::move(dir),
                                  file = std::move(file), tp = tp, &completion_latch]() {
                proceedCommand(std::move(command), dir, file, tp);
                completion_latch.count_down();
            });
        } else {
            if (!IsProcessingAllDirectory)
                generatePlainFile(projectManager, file);
            completion_latch.count_down();
            std::cerr << "Could not find commands for " << file << "\n";
        }
    }
    SPDLOG_INFO("Entry process done, wait for backbround threads");
    completion_latch.wait();
    SPDLOG_INFO("Backbround threads done");
}
