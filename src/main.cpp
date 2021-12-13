#include "Binary/binarySerializer.h"
#include "HeadersParser/Parser.h"
#include "Meta/DeclarationConverterVisitor.h"
#include "Meta/Filters/HandleExceptionalMetasFilter.h"
#include "Meta/Filters/HandleMethodsAndPropertiesWithSameNameFilter.h"
#include "Meta/Filters/MergeCategoriesFilter.h"
#include "Meta/Filters/ModulesBlocklist.h"
#include "Meta/Filters/RemoveDuplicateMembersFilter.h"
#include "Meta/Filters/ResolveGlobalNamesCollisionsFilter.h"
#include "TypeScript/DefinitionWriter.h"
#include "TypeScript/DocSetManager.h"
#include "Vue/VueComponentDefinitionWriter.h"
#include "JSExport/JSExportDefinitionWriter.h"
#include "Yaml/YamlSerializer.h"
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Tooling/Tooling.h>
#include <fstream>
#include <llvm/Support/Debug.h>
#include <llvm/Support/Path.h>
#include <pwd.h>
#include <sstream>

// Command line parameters
llvm::cl::opt<bool>   cla_verbose("verbose", llvm::cl::desc("Set verbose output mode"), llvm::cl::value_desc("bool"));
llvm::cl::opt<bool>   cla_strictIncludes("strict-includes", llvm::cl::desc("Set strict include headers for diagnostic purposes (usually when some metadata is not generated due to wrong import or include statement)"), llvm::cl::value_desc("bool"));
llvm::cl::opt<string> cla_outputUmbrellaHeaderFile("output-umbrella", llvm::cl::desc("Specify the output umbrella header file"), llvm::cl::value_desc("file_path"));
llvm::cl::opt<string> cla_inputUmbrellaHeaderFile("input-umbrella", llvm::cl::desc("Specify the input umbrella header file"), llvm::cl::value_desc("file_path"));
llvm::cl::opt<string> cla_outputYamlFolder("output-yaml", llvm::cl::desc("Specify the output yaml folder"), llvm::cl::value_desc("<dir_path>"));
llvm::cl::opt<string> cla_outputModuleMapsFolder("output-modulemaps", llvm::cl::desc("Specify the fodler where modulemap files of all parsed modules will be dumped"), llvm::cl::value_desc("<dir_path>"));
llvm::cl::opt<string> cla_outputBinFile("output-bin", llvm::cl::desc("Specify the output binary metadata file"), llvm::cl::value_desc("<file_path>"));
llvm::cl::opt<string> cla_outputDtsFolder("output-typescript", llvm::cl::desc("Specify the output .d.ts folder"), llvm::cl::value_desc("<dir_path>"));
llvm::cl::opt<string> cla_outputJSEFolder("output-jsexport", llvm::cl::desc("Specify the output folder for .swift JSExport files"), llvm::cl::value_desc("<dir_path>"));
llvm::cl::opt<string> cla_outputVueFolder("output-vue", llvm::cl::desc("Specify the output folder for .vue components"), llvm::cl::value_desc("<dir_path>"));
llvm::cl::opt<string> cla_docSetFile("docset-path", llvm::cl::desc("Specify the path to the iOS SDK docset package"), llvm::cl::value_desc("<file_path>"));
llvm::cl::opt<string> cla_blockListModuleRegexesFile("blocklist-modules-file", llvm::cl::desc("Specify the metadata entries blocklist file containing regexes of module names on each line"), llvm::cl::value_desc("file_path"));
llvm::cl::opt<string> cla_whiteListModuleRegexesFile("whitelist-modules-file", llvm::cl::desc("Specify the metadata entries whitelist file containing regexes of module names on each line"), llvm::cl::value_desc("file_path"));
llvm::cl::opt<bool>   cla_applyManualDtsChanges("apply-manual-dts-changes", llvm::cl::desc("Specify whether to disable manual adjustments to generated .d.ts files for specific erroneous cases in the iOS SDK"), llvm::cl::init(true));
llvm::cl::opt<string> cla_clangArgumentsDelimiter(llvm::cl::Positional, llvm::cl::desc("Xclang"), llvm::cl::init("-"));
llvm::cl::list<string> cla_clangArguments(llvm::cl::ConsumeAfter, llvm::cl::desc("<clang arguments>..."));

class MetaGenerationConsumer : public clang::ASTConsumer {
public:
  explicit MetaGenerationConsumer(clang::SourceManager& sourceManager, clang::HeaderSearch& headerSearch, Meta::ModulesBlocklist& modulesBlocklist)
  : _headerSearch(headerSearch)
  , _visitor(sourceManager, _headerSearch, cla_verbose, modulesBlocklist)
  {
  }

  virtual void HandleTranslationUnit(clang::ASTContext& Context) override
  {
    Context.getDiagnostics().Reset();
    llvm::SmallVector<clang::Module*, 64> modules;
    _headerSearch.collectAllModules(modules);

    cout << "Loading API notes...";
    
    for (clang::Module* module : modules) {
      Meta::Type::populateModule(module->getFullModuleName());
    }
    
    cout << " done." << endl;

    list<Meta::Meta*>& metaContainer = _visitor.generateMetadata(Context.getTranslationUnitDecl());

    // Filters
    Meta::HandleExceptionalMetasFilter().filter(metaContainer);
    Meta::MergeCategoriesFilter().filter(metaContainer);
    Meta::RemoveDuplicateMembersFilter().filter(metaContainer);
    Meta::HandleMethodsAndPropertiesWithSameNameFilter(_visitor.getMetaFactory()).filter(metaContainer);
    Meta::ResolveGlobalNamesCollisionsFilter filter = Meta::ResolveGlobalNamesCollisionsFilter();
    filter.filter(metaContainer);
    unique_ptr<pair<Meta::ResolveGlobalNamesCollisionsFilter::MetasByModules, Meta::ResolveGlobalNamesCollisionsFilter::InterfacesByName> > result = filter.getResult();
    Meta::ResolveGlobalNamesCollisionsFilter::MetasByModules& metasByModules = result->first;

    Meta::ResolveGlobalNamesCollisionsFilter::InterfacesByName& interfacesByName = result->second;
    _visitor.getMetaFactory().getTypeFactory().resolveCachedBridgedInterfaceTypes(interfacesByName);

    // Log statistic for parsed Meta objects
    cout << "Result: " << metaContainer.size() << " declarations from " << metasByModules.size() << " top level modules" << endl;

    // Dump module maps
//    if (!cla_outputModuleMapsFolder.empty()) {
//      llvm::sys::fs::create_directories(cla_outputModuleMapsFolder);
//      for (clang::Module*& module : modules) {
//        string filePath = string(cla_outputModuleMapsFolder) + string("/") + module->getFullModuleName() + ".modulemap";
//        error_code error;
//        llvm::raw_fd_ostream file(filePath, error, llvm::sys::fs::F_Text);
//        if (error) {
//          cout << error.message();
//          continue;
//        }
//        module->print(file);
//        file.close();
//      }
//    }

    // Serialize Meta objects to Yaml
    if (!cla_outputYamlFolder.empty()) {
      if (!llvm::sys::fs::exists(cla_outputYamlFolder)) {
        DEBUG_WITH_TYPE("yaml", llvm::dbgs() << "Creating YAML output directory: " << cla_outputYamlFolder << "\n");
        llvm::sys::fs::create_directories(cla_outputYamlFolder);
      }

      for (pair<clang::Module*, vector<Meta::Meta*> >& modulePair : metasByModules) {
        string yamlFileName = modulePair.first->getFullModuleName() + ".yaml";
        DEBUG_WITH_TYPE("yaml", llvm::dbgs() << "Generating: " << yamlFileName << "\n");
        Yaml::YamlSerializer::serialize<pair<clang::Module*, vector<Meta::Meta*> > >(cla_outputYamlFolder + "/" + yamlFileName, modulePair);
      }
    }

    // Serialize Meta objects to binary metadata
//    if (!cla_outputBinFile.empty()) {
//      binary::MetaFile file(metaContainer.size() / 10); // Average number of hash collisions: 10 per bucket
//      binary::BinarySerializer serializer(&file);
//      serializer.serializeContainer(metasByModules);
//      file.save(cla_outputBinFile);
//    }
//
    // Generate JSExport definitions
    if (!cla_outputJSEFolder.empty()) {
      llvm::sys::fs::create_directories(cla_outputJSEFolder);
      string docSetPath = cla_docSetFile.empty() ? "" : cla_docSetFile.getValue();

      for (pair<clang::Module*, vector<Meta::Meta*> >& modulePair : metasByModules) {
        cout<< "[JSExport] " << modulePair.first->Name << "... ";
        TypeScript::JSExportDefinitionWriter jsDefinitionWriter(modulePair, _visitor.getMetaFactory().getTypeFactory(), docSetPath);
        jsDefinitionWriter.write();
        cout << std::to_string(modulePair.second.size()) << " done" << endl;
      }
      
      cout << endl;
    }

    // Generate component definitions
    if (!cla_outputVueFolder.empty()) {
      llvm::sys::fs::create_directories(cla_outputVueFolder);
      string docSetPath = cla_docSetFile.empty() ? "" : cla_docSetFile.getValue();

      cout << "Generating Vue components..." << endl;

      for (pair<clang::Module*, vector<Meta::Meta*> >& modulePair : metasByModules) {
        cout << "[Vue] " << modulePair.first->Name << "... ";
        TypeScript::VueComponentDefinitionWriter vueDefinitionWriter(modulePair, _visitor.getMetaFactory().getTypeFactory(), docSetPath);
        vueDefinitionWriter.write();
        cout << std::to_string(modulePair.second.size()) << " done" << endl;
      }
    }

    // Generate TypeScript definitions
    if (!cla_outputDtsFolder.empty()) {
      ostringstream output;

      llvm::sys::fs::create_directories(cla_outputDtsFolder);
      string docSetPath = cla_docSetFile.empty() ? "" : cla_docSetFile.getValue();

      llvm::SmallString<128> path;
      llvm::sys::path::append(path, cla_outputDtsFolder, "MacOS.ts");
      error_code error;
      llvm::raw_fd_ostream file(path.str(), error, llvm::sys::fs::F_Text);

      if (error) {
        cout << error.message();
        return;
      }

      cout << "Generating TypeScript definitions..." << endl;

      output << "/* eslint-disable */\n\n";

      // These are all inside a declare block, so they are invisible
      // and runtime, we add the enums to the bridges classes later

      output << "declare global {\n\n";

      for (pair<clang::Module*, vector<Meta::Meta*> >& modulePair : metasByModules) {
        cout << "[Typescript] " << modulePair.first->Name << "... ";
        TypeScript::DefinitionWriter definitionWriter(modulePair, _visitor.getMetaFactory().getTypeFactory(), docSetPath);
        output << definitionWriter.visitAll();
        cout << std::to_string(modulePair.second.size()) << " done" << endl;
      }
      
      cout << endl;

      const char * namespaceFills = R"__literal(namespace AE {
      export enum AEDataModel { }
      })__literal";

      output << namespaceFills << endl;

      output << TypeScript::DefinitionWriter::writeNamespaces();

      output << "}\n\n";

      output << "// Add enums to the already-existing bridged classes\n";
      output << "//\n";
      output << "// If we didn't do this, these would be duplicated\n";
      output << "// (i.e. both NSButton and NSButton$1 would exist\n";
      output << "// in global scope)\n\n";

      output << "let global = globalThis as any;\n\n";

      output << TypeScript::DefinitionWriter::writeNamespaces(true);

      output << TypeScript::DefinitionWriter::writeExports();

      file << output.str();
      file.close();

      cout << "Wrote " << path.c_str() << endl << endl;
    }
  }

private:
  clang::HeaderSearch& _headerSearch;
  Meta::DeclarationConverterVisitor _visitor;
};

class MetaGenerationFrontendAction : public clang::ASTFrontendAction {
public:
  MetaGenerationFrontendAction(Meta::ModulesBlocklist& modulesBlocklist)
  : _modulesBlocklist(modulesBlocklist)
  {
  }

  virtual unique_ptr<clang::ASTConsumer> CreateASTConsumer(clang::CompilerInstance& Compiler, llvm::StringRef InFile) override
  {
    // Since in 4.0.1 'includeNotFound' errors are ignored for some reason
    // (even though the 'suppressIncludeNotFound' setting is false)
    // here we set this explicitly in order to keep the same behavior
    Compiler.getPreprocessor().SetSuppressIncludeNotFoundError(!cla_strictIncludes);

    return unique_ptr<clang::ASTConsumer>(new MetaGenerationConsumer(Compiler.getASTContext().getSourceManager(), Compiler.getPreprocessor().getHeaderSearchInfo(), _modulesBlocklist));
  }

private:
  Meta::ModulesBlocklist& _modulesBlocklist;
};

string replaceString(string subject, const string& search, const string& replace)
{
  size_t pos = 0;
  while ((pos = subject.find(search, pos)) != string::npos) {
    subject.replace(pos, search.length(), replace);
    pos += replace.length();
  }
  return subject;
}

static void dumpArgs(ostream& os, int argc, const char **argv, char **envp) {
//  for (char **env = envp; *env != 0; env++)
//  {
//    char *thisEnv = *env;
//    printf("%s\n", thisEnv);
//  }
  
  os << "Metadata Generator Arguments: " << endl;
  for (int i = 0; i < argc; ++i) {
    string arg = *(argv + i);
    os << arg << " ";
  }
  os << endl;
}

static void setBuildConfig() {
  string sdkVersion(getenv("SDKVERSION"));
  
  if (sdkVersion.empty()) {
    throw logic_error("Need to set SDKVERSION env var");
  }
  
  vector <string> tokens;
  stringstream sdkVersionStream(sdkVersion);
  string intermediate;
  
  while(getline(sdkVersionStream, intermediate, '.')) {
    tokens.push_back(intermediate);
  }
  
  if (tokens.size() > 0) {
    Meta::Utils::buildTarget.Major = (int)stoi(tokens[0]);
  }
  else {
    throw logic_error("Invalid SDKVersion");
  }
  
  if (tokens.size() > 1) {
    Meta::Utils::buildTarget.Minor = (int)stoi(tokens[1]);
  }
  else {
    throw logic_error("Invalid SDKVersion");
  }
  
  if (tokens.size() > 2) {
    Meta::Utils::buildTarget.SubMinor = (int)stoi(tokens[2]);
  }
  else {
    // default to target latest if only major/minor are set
    Meta::Utils::buildTarget.SubMinor = (int)INT_MAX;
  }

  string swiftVersion(getenv("SWIFTVERSION"));
  
  if (swiftVersion.empty()) {
    throw logic_error("Need to set SWIFTVERSION env var");
  }
  
  vector <string> tokensS;
  stringstream swiftVersionStream(swiftVersion);
  string intermediateS;
  
  while(getline(swiftVersionStream, intermediateS, '.')) {
    tokensS.push_back(intermediateS);
  }
  
  if (tokensS.size() > 0) {
    Meta::Utils::swiftVersion.Major = (int)stoi(tokensS[0]);
  }
  else {
    throw logic_error("Invalid SWIFTVERSION");
  }
  
  if (tokensS.size() > 1) {
    Meta::Utils::swiftVersion.Minor = (int)stoi(tokensS[1]);
  }
  else {
    throw logic_error("Invalid SWIFTVERSION");
  }
  
  if (tokensS.size() > 2) {
    Meta::Utils::swiftVersion.SubMinor = (int)stoi(tokensS[2]);
  }
  else {
    // default to target latest if only major/minor are set
    Meta::Utils::swiftVersion.SubMinor = (int)INT_MAX;
  }
}

int main(int argc, const char** argv, char **envp)
{
  try {
    freopen("stderr.log", "w", stderr);

    setBuildConfig();
    assert(Meta::Utils::buildTarget.Major > 0);

    clock_t begin = clock();

    llvm::cl::ParseCommandLineOptions(argc, argv);
    assert(cla_clangArgumentsDelimiter.getValue() == "Xclang");

    TypeScript::JSExportDefinitionWriter::outputJSEFolder = cla_outputJSEFolder.getValue();
    TypeScript::VueComponentDefinitionWriter::outputVueFolder = cla_outputVueFolder.getValue();

    // Log Metadata Genrator Arguments
    dumpArgs(cout, argc, argv, envp);
    dumpArgs(cerr, argc, argv, envp);

    TypeScript::DefinitionWriter::applyManualChanges = cla_applyManualDtsChanges;

    vector<string> clangArgs{
      "-v",
      "-x", "objective-c",
      "-fno-objc-arc", "-fmodule-maps", "-ferror-limit=0",
      "-Wno-unknown-pragmas", "-Wno-ignored-attributes", "-Wno-nullability-completeness", "-Wno-expansion-to-defined",
      "-D__NATIVESCRIPT_METADATA_GENERATOR=1"
    };

    // merge with hardcoded clang arguments
    clangArgs.insert(clangArgs.end(), cla_clangArguments.begin(), cla_clangArguments.end());

    // Log Clang Arguments
    cout << "Clang Arguments: \n";
    for (const string& arg : clangArgs) {
      cout << "\"" << arg << "\","
      << " ";
    }
    cout << endl;

    string isysroot;
    vector<string>::const_iterator it = find(clangArgs.begin(), clangArgs.end(), "-isysroot");
    if (it != clangArgs.end() && ++it != clangArgs.end()) {
      isysroot = *it;
    }

    vector<string> includePaths;
    string umbrellaContent = CreateUmbrellaHeader(clangArgs, includePaths);

    if (!cla_inputUmbrellaHeaderFile.empty()) {
      ifstream fs(cla_inputUmbrellaHeaderFile);
      umbrellaContent = string((istreambuf_iterator<char>(fs)),
                                    istreambuf_iterator<char>());
    }

    clangArgs.insert(clangArgs.end(), includePaths.begin(), includePaths.end());

    // Save the umbrella file
    if (!cla_outputUmbrellaHeaderFile.empty()) {
      error_code errorCode;
      llvm::raw_fd_ostream umbrellaFileStream(cla_outputUmbrellaHeaderFile, errorCode, llvm::sys::fs::OpenFlags::F_None);
      if (!errorCode) {
        umbrellaFileStream << umbrellaContent;
        umbrellaFileStream.close();
      }
    }
    // generate metadata for the intermediate sdk header
    Meta::ModulesBlocklist modulesBlocklist(cla_whiteListModuleRegexesFile, cla_blockListModuleRegexesFile);
    clang::tooling::runToolOnCodeWithArgs(new MetaGenerationFrontendAction(/*r*/modulesBlocklist), umbrellaContent, clangArgs, "umbrella.h", "objc-metadata-generator");

    clock_t end = clock();
    double elapsed_secs = double(end - begin) / CLOCKS_PER_SEC;
    cout << "Done! Running time: " << elapsed_secs << " sec " << endl;

    return 0;
  } catch (const exception& e) {
    cerr << "error: " << e.what() << endl;
    return 1;
  }
}
