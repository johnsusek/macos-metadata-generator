#pragma once

#include "TypeScript/DocSetManager.h"
#include "Meta/MetaEntities.h"
#include <Meta/TypeFactory.h>
#include <sstream>
#include <string>
#include <unordered_set>
#include "yaml-cpp/yaml.h"

namespace TypeScript {
class JSExportMeta {
public:
  static JSExportMeta current;
  static std::string outputJSEFolder;

  std::map<std::string, YAML::Node> attributesLookup;
  std::string lookupApiNotes(std::string type);
//  std::vector<std::string> getRenamedForSwift(const clang::ObjCMethodDecl* method, std::string categoryName);
//  std::vector<std::string> getRenamedForSwift(const clang::ObjCPropertyDecl* property, std::string categoryName);
  std::vector<std::string> getRenamedForSwift2(::Meta::Meta* meta, ::Meta::Meta* owner);
  bool getUnavailableInSwift(::Meta::Meta* meta, ::Meta::Meta* owner);
  bool populateModule(std::string moduleName);
  void getClosedGenericsIfAnyJS(::Meta::Type& type, std::vector<::Meta::Type*>& params);

private:
  std::map<std::string, std::string> notes;
  bool populateModuleAttrs(std::string moduleName);
};
}
