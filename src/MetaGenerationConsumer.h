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
#include <thread>

llvm::cl::opt<string> cla_outputYamlFolder("output-yaml", llvm::cl::desc("Specify the output yaml folder"), llvm::cl::value_desc("<dir_path>"));
llvm::cl::opt<string> cla_outputModuleMapsFolder("output-modulemaps", llvm::cl::desc("Specify the fodler where modulemap files of all parsed modules will be dumped"), llvm::cl::value_desc("<dir_path>"));
llvm::cl::opt<string> cla_outputBinFile("output-bin", llvm::cl::desc("Specify the output binary metadata file"), llvm::cl::value_desc("<file_path>"));
llvm::cl::opt<string> cla_outputDtsFolder("output-typescript", llvm::cl::desc("Specify the output .d.ts folder"), llvm::cl::value_desc("<dir_path>"));
llvm::cl::opt<string> cla_outputJSEFolder("output-jsexport", llvm::cl::desc("Specify the output folder for .swift JSExport files"), llvm::cl::value_desc("<dir_path>"));
llvm::cl::opt<string> cla_outputVueFolder("output-vue", llvm::cl::desc("Specify the output folder for .vue components"), llvm::cl::value_desc("<dir_path>"));
llvm::cl::opt<bool>   cla_verbose("verbose", llvm::cl::desc("Set verbose output mode"), llvm::cl::value_desc("bool"));
llvm::cl::opt<string> cla_docSetFile("docset-path", llvm::cl::desc("Specify the path to the iOS SDK docset package"), llvm::cl::value_desc("<file_path>"));

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
