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
#include "MetaGenerationConsumer.h"
#include <clang/Frontend/CompilerInstance.h>
#include <clang/Tooling/Tooling.h>
#include <fstream>
#include <llvm/Support/Debug.h>
#include <llvm/Support/Path.h>
#include <pwd.h>
#include <sstream>

llvm::cl::opt<bool>   cla_strictIncludes("strict-includes", llvm::cl::desc("Set strict include headers for diagnostic purposes (usually when some metadata is not generated due to wrong import or include statement)"), llvm::cl::value_desc("bool"));

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

