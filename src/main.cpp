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
#include "MetaGenerationFrontendAction.h"
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Tooling/Tooling.h>
#include <fstream>
#include <llvm/Support/Debug.h>
#include <llvm/Support/Path.h>
#include <pwd.h>
#include <sstream>

llvm::cl::opt<string> cla_outputUmbrellaHeaderFile("output-umbrella", llvm::cl::desc("Specify the output umbrella header file"), llvm::cl::value_desc("file_path"));
llvm::cl::opt<string> cla_inputUmbrellaHeaderFile("input-umbrella", llvm::cl::desc("Specify the input umbrella header file"), llvm::cl::value_desc("file_path"));
llvm::cl::opt<string> cla_blockListModuleRegexesFile("blocklist-modules-file", llvm::cl::desc("Specify the metadata entries blocklist file containing regexes of module names on each line"), llvm::cl::value_desc("file_path"));
llvm::cl::opt<string> cla_whiteListModuleRegexesFile("whitelist-modules-file", llvm::cl::desc("Specify the metadata entries whitelist file containing regexes of module names on each line"), llvm::cl::value_desc("file_path"));
llvm::cl::opt<bool>   cla_applyManualDtsChanges("apply-manual-dts-changes", llvm::cl::desc("Specify whether to disable manual adjustments to generated .d.ts files for specific erroneous cases in the iOS SDK"), llvm::cl::init(true));
llvm::cl::opt<string> cla_clangArgumentsDelimiter(llvm::cl::Positional, llvm::cl::desc("Xclang"), llvm::cl::init("-"));
llvm::cl::list<string> cla_clangArguments(llvm::cl::ConsumeAfter, llvm::cl::desc("<clang arguments>..."));

static void dumpArgs(ostream& os, int argc, const char **argv, char **envp) {
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

void run(int argc, const char** argv, char **envp)
{
  setBuildConfig();
  assert(Meta::Utils::buildTarget.Major > 0);
  
  clock_t begin = clock();
  
  llvm::cl::ParseCommandLineOptions(argc, argv);
  assert(cla_clangArgumentsDelimiter.getValue() == "Xclang");
  
  TypeScript::JSExportDefinitionWriter::outputJSEFolder = cla_outputJSEFolder.getValue();
  TypeScript::VueComponentDefinitionWriter::outputVueFolder = cla_outputVueFolder.getValue();
  
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
  
  clangArgs.insert(clangArgs.end(), cla_clangArguments.begin(), cla_clangArguments.end());
  
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

  Meta::ModulesBlocklist modulesBlocklist(cla_whiteListModuleRegexesFile, cla_blockListModuleRegexesFile);
  clang::tooling::runToolOnCodeWithArgs(new MetaGenerationFrontendAction(/*r*/modulesBlocklist), umbrellaContent, clangArgs, "umbrella.h", "objc-metadata-generator");
  
  clock_t end = clock();
  double elapsed_secs = double(end - begin) / CLOCKS_PER_SEC;
  cout << "Done! Running time: " << elapsed_secs << " sec " << endl;
}

int main(int argc, const char** argv, char **envp)
{
  try {
    freopen("stderr.log", "w", stderr);
    run(argc, argv, envp);
    return 0;
  } catch (const exception& e) {
    cerr << "error: " << e.what() << endl;
    return 1;
  }
}
