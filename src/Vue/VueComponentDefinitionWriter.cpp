#include "TypeScript/DefinitionWriter.h"
#include "VueComponentDefinitionWriter.h"
#include "VueComponentFormatter.h"
#include "Meta/MetaFactory.h"
#include "Meta/Utils.h"
#include "Meta/NameRetrieverVisitor.h"
#include "Utils/StringUtils.h"
#include <algorithm>
#include <iostream>
#include <cctype>
#include <clang/AST/DeclObjC.h>
#include <iterator>
#include <regex>
#include "yaml-cpp/yaml.h"
#include <cstdlib>

namespace TypeScript {
using namespace Meta;
using namespace std;

string VueComponentDefinitionWriter::outputVueFolder = "";

static unordered_set<string> hiddenMethods = {
  "alloc",
  "allocWith",
  "allocWithZone"
  "autorelease",
  "release",
  "retain",
  "new"
  "self",
  "zone",
  "class",
  "subscript"
};

static unordered_set<string> writtenProps = {};

string kebabCase(string camelCase) {
  string str(1, tolower(camelCase[0]));
  
  for (auto it = camelCase.begin() + 1; it != camelCase.end(); ++it) {
    if (isupper(*it) && *(it-1) != '-' && islower(*(it-1))) {
      str += "-";
    }
    str += *it;
  }
  
  transform(str.begin(), str.end(), str.begin(), ::tolower);
  
  return str;
}

inline bool endsWith(string const & value, string const & ending)
{
  if (ending.size() > value.size()) return false;
  return equal(ending.rbegin(), ending.rend(), value.rbegin());
}

string getPropsEntry(string returnType, PropertyMeta* meta = NULL) {
  ostringstream output;
  
  if (VueComponentFormatter::nativeTypes[returnType]) {
    returnType[0] = toupper(returnType[0]);
    output << "type: " << returnType;
  }
  else if (meta && meta->getter->signature[0]->getType() == TypeEnum) {
    output << "type: String as PropType<keyof typeof " << returnType << ">";
  }
  else if (returnType == "JSManagedValue") {
    output << "type: Function as PropType<() => void>";
  }
  else {
    if (returnType == "id[]") {
      returnType = "any[]";
    }
    output << "type: Object as PropType<" << returnType << ">";
  }
  
  return output.str();
}

// MARK: - Visit

// MARK: Interface

void VueComponentDefinitionWriter::visit(InterfaceMeta* meta)
{
  CompoundMemberMap<MethodMeta> compoundInstanceMethods;
  
  for (MethodMeta* method : meta->instanceMethods) {
    compoundInstanceMethods.emplace(method->name, make_pair(meta, method));
  }
  
  CompoundMemberMap<PropertyMeta> baseClassInstanceProperties;
  CompoundMemberMap<PropertyMeta> ownInstanceProperties;
  
  for (PropertyMeta* property : meta->instanceProperties) {
    if (ownInstanceProperties.find(property->name) == ownInstanceProperties.end()) {
      ownInstanceProperties.emplace(property->name, make_pair(meta, property));
    }
  }
  
  unordered_set<ProtocolMeta*> inheritedProtocols;
  CompoundMemberMap<PropertyMeta> protocolInheritedInstanceProperties;
  unordered_set<ProtocolMeta*> protocols;
  
  unordered_set<ProtocolMeta*> immediateProtocols;
  for (auto protocol : protocols) {
    if (inheritedProtocols.find(protocol) == inheritedProtocols.end()) {
      immediateProtocols.insert(protocol);
    }
  }
  //
  //  if (compoundInstanceMethods.empty() && ownInstanceProperties.empty()
  //      && protocolInheritedInstanceProperties.empty()) {
  //    return;
  //  }
  
  //
  // Write Vue props definition for instance properties + setters
  //
  
  _buffer << "  props: {\n";
  
  for (auto& propertyPair : ownInstanceProperties) {
    BaseClassMeta* owner = propertyPair.second.first;
    PropertyMeta* propertyMeta = propertyPair.second.second;
    
    if (owner == meta) {
      this->writeProperty(propertyMeta, owner, meta, baseClassInstanceProperties);
    }
  }
  
  for (auto& propertyPair : protocolInheritedInstanceProperties) {
    BaseClassMeta* owner = propertyPair.second.first;
    PropertyMeta* propertyMeta = propertyPair.second.second;
    
    bool isDuplicated = ownInstanceProperties.find(propertyMeta->jsName) != ownInstanceProperties.end();
    
    if (immediateProtocols.find(reinterpret_cast<ProtocolMeta*>(owner)) != immediateProtocols.end() && !isDuplicated) {
      this->writeProperty(propertyMeta, owner, meta, baseClassInstanceProperties);
    }
  }
  
  for (auto& methodPair : compoundInstanceMethods) {
    if (ownInstanceProperties.find(methodPair.first) != ownInstanceProperties.end()) {
      continue;
    }
    
    MethodMeta* method = methodPair.second.second;
    
    if (method->getFlags(MethodIsInitializer)) {
      continue;
    }
    
    string output = writeMethod(methodPair, meta, immediateProtocols);
    
    if (output.size()) {
      _buffer << "  ";
      _buffer << _docSet.getCommentFor(methodPair.second.second, methodPair.second.first).toString("  ");
      _buffer << output;
    }
  }
  
  _buffer << "  },\n\n";
  
  _buffer << "  computed: {\n";
  _buffer << "    attrs() {\n";
  _buffer << "      let attrs: any = {};\n\n";
  _buffer << "      let types = {\n";

  //
  // Write computed attrs (e.g. enum values from vue prop string)
  //
  
  for (auto& propertyPair : ownInstanceProperties) {
    BaseClassMeta* owner = propertyPair.second.first;
    PropertyMeta* propertyMeta = propertyPair.second.second;
    
    if (owner == meta) {
      string output = this->writePropertyComputed(propertyMeta, owner);
      if (output.size()) {
        _buffer << output;
      }
    }
  }
  
  for (auto& propertyPair : protocolInheritedInstanceProperties) {
    BaseClassMeta* owner = propertyPair.second.first;
    PropertyMeta* propertyMeta = propertyPair.second.second;
    
    bool isDuplicated = ownInstanceProperties.find(propertyMeta->jsName) != ownInstanceProperties.end();
    if (immediateProtocols.find(reinterpret_cast<ProtocolMeta*>(owner)) != immediateProtocols.end() && !isDuplicated) {
      string output = this->writePropertyComputed(propertyMeta, owner);
      if (output.size()) {
        _buffer << output;
      }
    }
  }
  
  for (auto& methodPair : compoundInstanceMethods) {
    if (ownInstanceProperties.find(methodPair.first) != ownInstanceProperties.end()) {
      continue;
    }
    
    MethodMeta* method = methodPair.second.second;
    BaseClassMeta* owner = methodPair.second.first;
    
    if (method->getFlags(MethodIsInitializer)) {
      continue;
    }

    string output = writeMethodComputed(method, owner);
    
    if (output.size()) {
      _buffer << output;
    }
  }

  _buffer << "      };\n\n";
  
  _buffer << "      for (const [propName, propType] of Object.entries(types)) {\n";
  _buffer << "        if (this[propName] !== undefined) {\n";
  _buffer << "          attrs[propName] = propType[this[propName]];\n";
  _buffer << "        }\n";
  _buffer << "      }\n\n";

  _buffer << "      return { ...this.$props, ...this.$attrs, ...attrs };\n    }\n  },\n\n";
  
  //
  // Vue render fn
  //
  // Here we want to use our NSView subclass name for h(...),
  // (i.e. StackView instead of NSStackView)
  //
  // This gets passed as the first param to createElement()
  // in createRenderer.ts, which will call a .create* method
  // on the passed class name
  //
  
  _buffer << "  render() {\n";
  
  regex frameworkPrefixes("^(NS|AV|IK)");
  string noprefixName = regex_replace(meta->jsName, frameworkPrefixes, "");
  
  _buffer << "    return h('" << noprefixName << "', this.attrs, this.$slots);\n";
  _buffer << "  }\n";
}

void VueComponentDefinitionWriter::visit(ProtocolMeta* meta)
{
}

void VueComponentDefinitionWriter::visit(CategoryMeta* meta)
{
}

void VueComponentDefinitionWriter::visit(FunctionMeta* meta)
{
}

void VueComponentDefinitionWriter::visit(StructMeta* meta)
{
}

void VueComponentDefinitionWriter::visit(UnionMeta* meta)
{
}

void VueComponentDefinitionWriter::visit(EnumMeta* meta)
{
}

void VueComponentDefinitionWriter::visit(VarMeta* meta)
{
}

void VueComponentDefinitionWriter::visit(MethodMeta* meta)
{
}

void VueComponentDefinitionWriter::visit(PropertyMeta* meta)
{
}

void VueComponentDefinitionWriter::visit(EnumConstantMeta* meta)
{
}

// MARK: - Write

string VueComponentDefinitionWriter::writeMethod(CompoundMemberMap<MethodMeta>::value_type& methodPair, BaseClassMeta* owner, const unordered_set<ProtocolMeta*>& protocols, string keyword)
{
  string output;
  
  BaseClassMeta* memberOwner = methodPair.second.first;
  MethodMeta* method = methodPair.second.second;
  
  if (hiddenMethods.find(method->jsName) != hiddenMethods.end()) {
    return string();
  }
  
  bool isOwnMethod = memberOwner == owner;
  bool implementsProtocol = protocols.find(static_cast<ProtocolMeta*>(memberOwner)) != protocols.end();
  bool returnsInstanceType = method->signature[0]->is(TypeInstancetype);
  
  if (isOwnMethod || implementsProtocol || returnsInstanceType) {
    output = writeMethod(method, owner, keyword);
  }
  
  return output;
}

string VueComponentDefinitionWriter::writeMethod(MethodMeta* method, BaseClassMeta* owner, string keyword)
{
  ostringstream output;
  
  const clang::ObjCMethodDecl& methodDecl = *clang::dyn_cast<clang::ObjCMethodDecl>(method->declaration);
  
  string name = method->jsName;
  
  if (!keyword.empty()) {
    cout << "Skipping prop " + name + " because it has a keyword " + keyword << endl;
    return output.str();
  }
  
  // We are only interested in setters (w/ single param)
  // when writing methods for Vue components
  if (name.substr(0, 3) != "set" || method->signature.size() != 2 || name == "set") {
    return output.str();
  }
  
  string nameWithoutSet = name.substr(3);
  nameWithoutSet[0] = tolower(nameWithoutSet[0]);
  
  string kebabName = kebabCase(nameWithoutSet);
  
  if (writtenProps.find(kebabName) != writtenProps.end()) {
    return output.str();
  }
  
  writtenProps.insert(kebabName);
  
  output << "  '" << kebabName << "': {\n";
  
  string setterType = VueComponentFormatter::current.getTypeString(methodDecl.getASTContext(), methodDecl.getObjCDeclQualifier(), methodDecl.getReturnType(), *method->signature[1], false);
  
  setterType = VueComponentFormatter::current.vuePropifyTypeName(setterType);
  
  output << "      ";
  
  output << getPropsEntry(setterType) << ",\n";
  
  output << "      ";
  
  output << "default: () => undefined\n";
  
  output << "    },\n";
  
  return output.str();
}

string VueComponentDefinitionWriter::writeMethodComputed(MethodMeta* method, BaseClassMeta* owner)
{
  ostringstream output;
  const clang::ObjCMethodDecl& methodDecl = *clang::dyn_cast<clang::ObjCMethodDecl>(method->declaration);
  string name = method->jsName;
  
  // We are only interested in setters (w/ single param)
  // when writing methods for Vue components
  if (name.substr(0, 3) != "set" || method->signature.size() != 2 || name == "set") {
    return output.str();
  }
  
  string nameWithoutSet = name.substr(3);
  nameWithoutSet[0] = tolower(nameWithoutSet[0]);
  
  string kebabName = kebabCase(nameWithoutSet);
  string setterType = VueComponentFormatter::current.getTypeString(methodDecl.getASTContext(), methodDecl.getObjCDeclQualifier(), methodDecl.getReturnType(), *method->signature[1], false);
  setterType = VueComponentFormatter::current.vuePropifyTypeName(setterType);
  
  // Native types like String and Number don't get any special treatment
  if (VueComponentFormatter::nativeTypes[setterType]) {
    return output.str();
  }
  
  if (method && method->signature[1]->getType() == TypeEnum) {
    // convert prop string to enum value
    //    output << "      if (this." << nameWithoutSet << " !== undefined) {\n";
    //    output << "        attrs." << nameWithoutSet << " = " << setterType << "[this." << nameWithoutSet << "];\n";
    //    output << "      }\n\n";
    output << "        " << nameWithoutSet << ": " << setterType << ",\n";
  }
  
  return output.str();
}

void VueComponentDefinitionWriter::writeProperty(PropertyMeta* propertyMeta, BaseClassMeta* owner, InterfaceMeta* target, CompoundMemberMap<PropertyMeta> baseClassProperties)
{
  if (hiddenMethods.find(propertyMeta->jsName) != hiddenMethods.end()) {
    return;
  }
  
  const clang::ObjCPropertyDecl& propDecl = *clang::dyn_cast<clang::ObjCPropertyDecl>(propertyMeta->declaration);
  
  if (propDecl.isClassProperty()) {
    return;
  }
  
  bool optOutTypeChecking = false;
  auto result = baseClassProperties.find(propertyMeta->jsName);
  
  if (result != baseClassProperties.end()) {
    optOutTypeChecking = result->second.second->getter->signature[0] != propertyMeta->getter->signature[0];
  }
  
  _buffer << writeProperty(propertyMeta, target, optOutTypeChecking);
}

string VueComponentDefinitionWriter::writeProperty(PropertyMeta* meta, BaseClassMeta* owner, bool optOutTypeChecking)
{
  ostringstream output;
  
  if (!meta->setter) {
    return output.str();
  }
  
  string name = kebabCase(meta->jsName);
  
  writtenProps.insert(name);
  
  auto decl = clang::dyn_cast<clang::ObjCPropertyDecl>(meta->declaration);

  string returnType = VueComponentFormatter::current.formatType(*meta->getter->signature[0], decl->getType());
  returnType = VueComponentFormatter::current.vuePropifyTypeName(returnType);

  string retType2 = Type::tsifyType(*meta->getter->signature[0], false, true);
  
  retType2 = VueComponentFormatter::current.vuePropifyTypeName(retType2);
  retType2[0] = toupper(retType2[0]);


  if (retType2 != returnType && retType2 != "any" && returnType != "JSManagedValue" && retType2 != "String") {
    if (retType2 == "Any" && returnType != "any") {
      
    }
    else {
//      cout << "Using " << retType2 << " instead of " << returnType << endl;
      returnType = retType2;
    }
  }
  
  output << "    '" << name << "': {\n";
  output << "      ";
  output << getPropsEntry(returnType, meta) << ",\n";
  output << "      ";
  output << "default: () => undefined\n";
  output << "    },\n";
  
  return output.str();
}

string VueComponentDefinitionWriter::writePropertyComputed(PropertyMeta* meta, BaseClassMeta* owner)
{
  ostringstream output;
  
  // Can't set a value on read-only properties
  if (!meta->setter) {
    return output.str();
  }
  
  auto decl = clang::dyn_cast<clang::ObjCPropertyDecl>(meta->declaration);
  string propertyType = VueComponentFormatter::current.formatType(*meta->getter->signature[0], decl->getType());
  propertyType = VueComponentFormatter::current.vuePropifyTypeName(propertyType);
  
  string retType2 = Type::tsifyType(*meta->getter->signature[0], false, true);
  retType2 = VueComponentFormatter::current.vuePropifyTypeName(retType2);
  retType2[0] = toupper(retType2[0]);

  if (retType2 != propertyType && retType2 != "any" && propertyType != "JSManagedValue" && retType2 != "String") {
    if (retType2 == "Any" && propertyType != "any") {
      
    }
    else {
//      cout << "Using " << retType2 << " instead of " << propertyType << endl;
      propertyType = retType2;
    }
  }

  string name = meta->jsName;
  string kebabName = kebabCase(meta->jsName);
  
  // Native types like String and Number don't get any special treatment
  if (VueComponentFormatter::nativeTypes[propertyType]) {
    return output.str();
  }
  
  if (meta && meta->getter->signature[0]->getType() == TypeEnum) {
    // convert prop string to enum value
    //    output << "      if (this." << name << " !== undefined) {\n";
    //    output << "        attrs." << name << " = " << propertyType << "[this." << name << "];\n";
    //    output << "      }\n\n";
    output << "        " << name << ": " << propertyType << ",\n";
  }
  
  return output.str();
}

string VueComponentDefinitionWriter::write()
{
  _buffer.clear();
  _importedModules.clear();
  
  for (::Meta::Meta* meta : _module.second) {
    meta->visit(this);
    
    if (meta->is(MetaType::Interface)) {
      auto interface = static_cast<InterfaceMeta*>(meta);
      
      if (interface->isSubclassOf("NSView")
          || interface->jsName == "NSView"
          || interface->jsName == "NSTableColumn") {
        writeVueComponent(meta, _module.first->Name);
      }
    }
    
    _buffer.str("");
    _buffer.clear();
  }
  
  return "";
}

void VueComponentDefinitionWriter::writeVueComponent(::Meta::Meta* meta, string frameworkName)
{
  auto buffer = _buffer.str();
  if (buffer.empty() || buffer == "\n" || meta->jsName[0] == '_') { return; }
  
  string jsPath = outputVueFolder + "/" + frameworkName + "/";
  string mkdirCmd = "mkdir -p '" + jsPath + "'";
  const size_t mkdirReturn = system(mkdirCmd.c_str());
  
  if (mkdirReturn < 0)  {
    cout << "Error creating directory using command: " << mkdirCmd << endl;
    return;
  }
  
  error_code writeError;
  auto interface = static_cast<InterfaceMeta*>(meta);
  string moduleName = interface->module->getTopLevelModule()->Name;
  string baseModuleName = interface->base->module->getTopLevelModule()->Name;
  regex frameworkPrefixes("^NS");
  string pascalName = regex_replace(interface->jsName, frameworkPrefixes, "");
  string shortName = pascalName;
  
  if (shortName != "View") {
    regex viewSuffix("View$");
    shortName = regex_replace(pascalName, viewSuffix, "");
  }

  llvm::raw_fd_ostream jsFile(jsPath + shortName + ".vue", writeError, llvm::sys::fs::F_Text);
  
  if (writeError) {
    cout << writeError.message();
    return;
  }
  
  jsFile << "<script lang='ts'>\n";
  jsFile << "import { PropType, h, defineComponent } from '@vue/runtime-core';\n";
  
  string noprefixBasename = regex_replace(interface->base->jsName, frameworkPrefixes, "");

  string basePath = ".";

  if (moduleName != baseModuleName) {
    basePath = "../" + baseModuleName;
  }
  
  if (interface->base->isSubclassOf("NSView") || interface->base->jsName == "NSView") {
    jsFile << "import " << noprefixBasename << " from '" << basePath << "/" << noprefixBasename << ".vue';\n";
  }
  
  jsFile << "\n";
  jsFile << "export default defineComponent({\n";
  
  jsFile << "  name: '" << shortName << "',\n\n";

  if (interface->base->isSubclassOf("NSView") || interface->base->jsName == "NSView") {
    jsFile << "  extends: { " + noprefixBasename + " },\n\n";
  }
  
  jsFile << buffer;
  
  jsFile << "});\n";
  jsFile << "</script>\n";
  
  jsFile.close();
  
//  cout << "Wrote " << frameworkName + "/" + shortName + ".vue" << endl;
}
}

