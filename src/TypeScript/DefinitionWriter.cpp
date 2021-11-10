#include "DefinitionWriter.h"
#include "Meta/Utils.h"
#include "Meta/NameRetrieverVisitor.h"
#include "Meta/MetaFactory.h"
#include "Utils/StringUtils.h"
#include "JSExport/JSExportDefinitionWriter.h"
#include "Vue/VueComponentFormatter.h"
#include "yaml-cpp/yaml.h"
#include <algorithm>
#include <clang/AST/DeclObjC.h>
#include <iterator>

namespace TypeScript {
using namespace Meta;
using namespace std;

static map<string, map<string, map<string, vector<EnumMeta*>>>> namespaceEnums = {};
static map<string, vector<VarMeta*>> namespaceVars = {};
static map<string, vector<string>> namespaceClasses = {};
static map<string, bool> allNamespaces = {};
static map<string, bool> allClasses = {};
static map<string, bool> allInterfaces = {};
static map<string, map<string, string>> namespaceTypealiases = {};

static unordered_set<string> hiddenMethods = {
  "retain",
  "release",
  "autorelease",
  "allocWithZone",
  "zone",
  "string",
  "countByEnumeratingWithStateObjectsCount",
  "create"
};

static unordered_set<string> ignoredNamespaces = {
  "Array",
  "Set",
  "String",
  "Date",
  "Dictionary",
  "Error"
};

static unordered_set<string> bannedIdentifiers = {
  "arguments",
  "break",
  "case",
  "catch",
  "class",
  "const",
  "continue",
  "debugger",
  "default",
  "delete",
  "do",
  "else",
  "export",
  "extends",
  "finally",
  "for",
  "function",
  "if",
  "import",
  "in",
  "instanceof",
  "new",
  "null",
  "object",
  "return",
  "super",
  "switch",
  "this",
  "throw",
  "try",
  "typeof",
  "var",
  "void",
  "while",
  "with",
  "yield"
};

string DefinitionWriter::jsifyTypeName(const string& jsName)
{
  static map<string, string> jsNames = {
    { "Decimal", "number" },
    { "CGFloat", "number" },
    { "Float", "number" },
    { "Double", "number" },
    { "NSInteger", "number" },
    { "NSUInteger", "number" },
    { "Int", "number" },
    { "Int8", "number" },
    { "Int16", "number" },
    { "Int32", "number" },
    { "Int64", "number" },
    { "UInt", "number" },
    { "UInt8", "number" },
    { "UInt16", "number" },
    { "UInt32", "number" },
    { "UInt64", "number" },
    { "StaticString", "string" },
    { "NSString", "string" },
    { "Selector", "string" },
    { "Any", "any" },
    { "AnyClass", "any" },
    { "AnyObject", "any" },
    { "Bool", "boolean" }
  };
  
  if (!jsNames[jsName].empty()) {
    return jsNames[jsName];
  }
  
  return jsName;
}

bool DefinitionWriter::applyManualChanges = false;

string dataRoot = getenv("DATAPATH");
//
//void DefinitionWriter::populateStructsMeta()
//{
//  string structsLookupPath = dataRoot + "/structs.json";
//
//  auto structsNode = YAML::LoadFile(structsLookupPath);
//
//  for (YAML::const_iterator it = structsNode.begin(); it != structsNode.end(); ++it) {
//    string outerName = it->first.as<string>();
//    if (outerName == "Any") {
//      continue;
//    }
//    for (YAML::const_iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
//      string moduleName = it2->first.as<string>();
//      for (YAML::const_iterator it3 = it2->second.begin(); it3 != it2->second.end(); ++it3) {
//        string containerName = it3->first.as<string>();
//        for (YAML::const_iterator it4 = it3->second.begin(); it4 != it3->second.end(); ++it4) {
//          string className = it4->first.as<string>();
//          allNamespaces[renamedName(outerName)] = true;
//          allNamespaces[renamedName(moduleName)] = true;
//          allNamespaces[renamedName(containerName)] = true;
//          allNamespaces[renamedName(className)] = true;
//          cout << renamedName(moduleName) << "." << renamedName(containerName) << "." << renamedName(className) << endl;
//          structsMeta[renamedName(moduleName)][renamedName(containerName)][renamedName(className)] = it4->second;
//        }
//      }
//    }
//  }
//}

void populateTypealiases()
{
  string aliasesPath = dataRoot + "/aliases.json";
  
  auto aliasNode = YAML::LoadFile(aliasesPath);
  
  for (YAML::const_iterator it = aliasNode.begin(); it != aliasNode.end(); ++it) {
    string moduleName = it->first.as<string>();
    
    for (YAML::const_iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
      string f = it2->first.as<string>();
      string s = it2->second.as<string>();
      
      namespaceTypealiases[renamedName(moduleName)][f] = s;
    }
  }
}

static string sanitizeParameterName(const string& parameterName)
{
  if (bannedIdentifiers.find(parameterName) != bannedIdentifiers.end()) {
    return parameterName + "_";
  }
  else {
    return parameterName;
  }
}

static string getTypeParametersStringOrEmpty(const clang::ObjCInterfaceDecl* interfaceDecl)
{
  ostringstream output;
  if (clang::ObjCTypeParamList* typeParameters = interfaceDecl->getTypeParamListAsWritten()) {
    if (typeParameters->size()) {
      output << "<";
      for (unsigned i = 0; i < typeParameters->size(); i++) {
        clang::ObjCTypeParamDecl* typeParam = *(typeParameters->begin() + i);
        output << typeParam->getNameAsString();
        if (i < typeParameters->size() - 1) {
          output << ", ";
        }
      }
      output << ">";
    }
  }
  
  return output.str();
}

static vector<string> getTypeParameterNames(const clang::ObjCInterfaceDecl* interfaceDecl)
{
  vector<string> params;
  if (clang::ObjCTypeParamList* typeParameters = interfaceDecl->getTypeParamListAsWritten()) {
    if (typeParameters->size()) {
      for (unsigned i = 0; i < typeParameters->size(); i++) {
        clang::ObjCTypeParamDecl* typeParam = *(typeParameters->begin() + i);
        params.push_back(typeParam->getNameAsString());
      }
    }
  }
  return params;
}

string DefinitionWriter::getTypeArgumentsStringOrEmpty(const clang::ObjCObjectType* objectType)
{
  ostringstream output;
  llvm::ArrayRef<clang::QualType> typeArgs = objectType->getTypeArgsAsWritten();
  if (!typeArgs.empty()) {
    output << "<";
    for (unsigned i = 0; i < typeArgs.size(); i++) {
      auto typeName = VueComponentFormatter::current.formatType(*_typeFactory.create(typeArgs[i]), typeArgs[i]);
      output << typeName;
      if (i < typeArgs.size() - 1) {
        output << ", ";
      }
    }
    output << ">";
  }
  else {
    /* Fill implicit id parameters in similar cases:
     * @interface MyInterface<ObjectType1, ObjectType2>
     * @interface MyDerivedInterface : MyInterface
     */
    if (clang::ObjCTypeParamList* typeParameters = objectType->getInterface()->getTypeParamListAsWritten()) {
      if (typeParameters->size()) {
        output << "<";
        for (unsigned i = 0; i < typeParameters->size(); i++) {
          output << "NSObject";
          if (i < typeParameters->size() - 1) {
            output << ", ";
          }
        }
        output << ">";
      }
    }
  }
  
  return output.str();
}

// MARK: - Visit Interface

void DefinitionWriter::visit(InterfaceMeta* meta)
{
  ostringstream out;

  CompoundMemberMap<MethodMeta> compoundStaticMethods;
  for (MethodMeta* method : meta->staticMethods) {
    compoundStaticMethods.emplace(method->name, make_pair(meta, method));
  }
  
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
  
  CompoundMemberMap<PropertyMeta> baseClassStaticProperties;
  CompoundMemberMap<PropertyMeta> ownStaticProperties;
  for (PropertyMeta* property : meta->staticProperties) {
    if (ownStaticProperties.find(property->name) == ownStaticProperties.end()) {
      ownStaticProperties.emplace(property->name, make_pair(meta, property));
    }
  }
  
  unordered_set<ProtocolMeta*> inheritedProtocols;
  
  CompoundMemberMap<MethodMeta> inheritedStaticMethods;
  
  getInheritedMembersRecursive(meta, &inheritedStaticMethods, nullptr, nullptr, nullptr);
  
  for (auto& methodPair : inheritedStaticMethods) {
    MethodMeta* method = methodPair.second.second;
    if (!method->signature[0]->is(TypeInstancetype)) {
      continue;
    }
    if (compoundStaticMethods.find(method->name) != compoundStaticMethods.end()) {
      continue;
    }
    compoundStaticMethods.emplace(methodPair);
  }

  string metaName = meta->jsName;

  // TODO: Need to separate into ES6 modules to prevent overlaps like this
  if (metaName == "Port" && meta->module->getTopLevelModule()->Name == "AVFoundation") {
    return;
  }
  
  if (metaName == "Set" || metaName == "String" || metaName == "NSObject") {
    return;
  }
  
  string containerName = metaName;
  
  string fullMetaName = renamedName(meta->jsName);
  vector<string> metaNameTokens;
  StringUtils::split(fullMetaName, '.', back_inserter(metaNameTokens));
  
  if (metaNameTokens.size() == 2) {
    containerName = metaNameTokens[0];
    metaName = metaNameTokens[1];
  }
  
  string parametersString = getTypeParametersStringOrEmpty(clang::cast<clang::ObjCInterfaceDecl>(meta->declaration));
  
  // These conflict with TS global types
  if (metaName == "Date" || metaName == "Error" || metaName == "Array" || metaName == "NSMutableString" || metaName == "NSSimpleCString") {
    out << "// @ts-ignore\n";
  }
  
  if (metaName == "Date" || metaName == "Error") {
    out << "type " << metaName << " = NS" << metaName << "\n";
    out << "export class NS" << metaName;
  }
  else {
    out << "export class " << metaName;
  }
  
  out << parametersString;
  
  if (meta->base != nullptr) {
    out << " extends " << meta->base->jsName << getTypeArgumentsStringOrEmpty(clang::cast<clang::ObjCInterfaceDecl>(meta->declaration)->getSuperClassType());
  }
  
  CompoundMemberMap<PropertyMeta> protocolInheritedStaticProperties;
  CompoundMemberMap<PropertyMeta> protocolInheritedInstanceProperties;
  unordered_set<ProtocolMeta*> protocols;
  vector<ProtocolMeta*> metaProtocols;
  
  copy_if (meta->protocols.begin(), meta->protocols.end(), back_inserter(metaProtocols), [](ProtocolMeta* meta){
    set<string> protosToSkip = {
      "NSFastEnumeration",
      "NSSecureCoding",
      "NSCoding",
      "NSCopying",
      "NSAccessibility"
    };
    string protoName = meta->jsName;
    if (protoName == "NSUserInterfaceItemIdentification") {
      return true;
    }
    
    return false;
//    if (protosToSkip.find(protoName) != protosToSkip.end() || protoName.substr(0, 15) == "NSAccessibility") {
//      return false;
//    }
//    return true;
  } );

  if (metaProtocols.size()) {
    out << " implements ";
    
    for (size_t i = 0; i < metaProtocols.size(); i++) {
      string protoName = metaProtocols[i]->jsName;
      
      getProtocolMembersRecursive(metaProtocols[i], &compoundStaticMethods, &compoundInstanceMethods, &protocolInheritedStaticProperties, &protocolInheritedInstanceProperties, protocols);
      
      out << protoName;
      
      if (i < metaProtocols.size() - 1) {
        out << ", ";
      }
    }
  }
  
  out << " {" << endl;

  unordered_set<ProtocolMeta*> immediateProtocols;
  
  for (auto protocol : protocols) {
    if (inheritedProtocols.find(protocol) == inheritedProtocols.end()) {
      immediateProtocols.insert(protocol);
    }
  }
  
  for (auto& methodPair : compoundStaticMethods) {
    string output = writeMethod(methodPair, meta, immediateProtocols);

    if (output.size()) {
//      out << "  // compoundStaticMethods\n";

      if (ownStaticProperties.find(methodPair.second.second->jsName) != ownStaticProperties.end()) {
        out << "  //";
      }
      
      if (compoundInstanceMethods.find(methodPair.first) != compoundInstanceMethods.end()) {
        out << "  //";
      }
      
      if (inheritedStaticMethods.find(methodPair.first) != inheritedStaticMethods.end()) {
        out << "  //";
      }
      
      if (ownInstanceProperties.find(methodPair.second.second->jsName) != ownInstanceProperties.end()) {
        out << "  //";
      }
      
      out << "  static " << output << endl;
    }
  }
  
  for (auto& propertyPair : ownInstanceProperties) {
    BaseClassMeta* owner = propertyPair.second.first;
    PropertyMeta* propertyMeta = propertyPair.second.second;
    string propOut = this->writeProperty(propertyMeta, owner, meta, baseClassInstanceProperties);
    
    if (!propOut.empty()) {
//      out << "  // ownInstanceProperties\n";

      // if this (selectedCell) ownInstanceProperties exists in parent class's methods (selectedCell())
      if (protocolInheritedStaticProperties.find(propertyMeta->name) != protocolInheritedStaticProperties.end()) {
        out << "  // ";
      }
      if (compoundInstanceMethods.find(propertyMeta->name) != compoundInstanceMethods.end()) {
        out << "  // ";
      }
      if (compoundInstanceMethods.find(propertyMeta->name) != compoundInstanceMethods.end()) {
        out << "  // ";
      }

      if (meta->nameExistsInSuperclass(propertyMeta->name, Method)) {
        cerr << "Skipping property that exists as method in " << meta->jsName << " superclass: `" << propertyMeta->name << "`" << endl;
        continue;
      }
      
//      if (meta->nameExistsInSuperclass(propertyMeta->name, Property)) {
//        cerr << "Skipping property that exists as property in " << meta->jsName << " superclass: `" << propertyMeta->name << "`" << endl;
//        continue;
//      }
      
      if (compoundStaticMethods.find(propertyMeta->name) != compoundStaticMethods.end()) {
        out << "  // ";
      }
      if (baseClassStaticProperties.find(propertyMeta->name) != baseClassStaticProperties.end()) {
        out << "  // ";
      }
      if (inheritedStaticMethods.find(propertyMeta->name) != inheritedStaticMethods.end()) {
        out << "  // ";
      }
      if (owner != meta) {
        out << "  // ";
      }

      out << propOut;
    }
  }
  
  for (auto& propertyPair : ownStaticProperties) {
    BaseClassMeta* owner = propertyPair.second.first;
    PropertyMeta* propertyMeta = propertyPair.second.second;
    
//   out << "  // ownStaticProperties\n";

    if (protocolInheritedStaticProperties.find(propertyMeta->name) != protocolInheritedStaticProperties.end()) {
      out << "  // ";
    }
    if (ownInstanceProperties.find(propertyMeta->name) != ownInstanceProperties.end()) {
      out << "  // ";
    }
    if (compoundStaticMethods.find(propertyMeta->name) != compoundStaticMethods.end()) {
      out << "  // ";
    }
    if (baseClassStaticProperties.find(propertyMeta->name) != baseClassStaticProperties.end()) {
      out << "  // ";
    }
    if (inheritedStaticMethods.find(propertyMeta->name) != inheritedStaticMethods.end()) {
      out << "  // ";
    }
    if (owner != meta) {
      out << "  // ";
    }

    out << this->writeProperty(propertyMeta, owner, meta, baseClassInstanceProperties);
  }
  
  for (auto& propertyPair : protocolInheritedInstanceProperties) {
    BaseClassMeta* owner = propertyPair.second.first;
    PropertyMeta* propertyMeta = propertyPair.second.second;
    
//     out << "  // protocolInheritedInstanceProperties\n";

    bool isDuplicated = ownInstanceProperties.find(propertyMeta->name) != ownInstanceProperties.end();
    
    if (immediateProtocols.find(reinterpret_cast<ProtocolMeta*>(owner)) == immediateProtocols.end() || isDuplicated) {
      out << "  // ";
    }
    
    out << this->writeProperty(propertyMeta, owner, meta, baseClassInstanceProperties);
  }
  
  for (auto& propertyPair : protocolInheritedStaticProperties) {
    BaseClassMeta* owner = propertyPair.second.first;
    PropertyMeta* propertyMeta = propertyPair.second.second;

//    out << "  // protocolInheritedStaticProperties\n";
    out << this->writeProperty(propertyMeta, owner, meta, baseClassStaticProperties);
  }
  
//  auto objectAtIndexedSubscript = compoundInstanceMethods.find("objectAtIndexedSubscript");
//  if (objectAtIndexedSubscript != compoundInstanceMethods.end()) {
//    const Type* retType = objectAtIndexedSubscript->second.second->signature[0];
//    string indexerReturnType = computeMethodReturnType(retType, meta, true);
//    out << "  [index: number]: " << indexerReturnType << ";" << endl;
//  }
//
//  if (compoundInstanceMethods.find("countByEnumeratingWithStateObjectsCount") != compoundInstanceMethods.end()) {
//    out << "  [Symbol.iterator](): Iterator<any>;" << endl;
//  }
  
  for (auto& methodPair : compoundInstanceMethods) {
    BaseClassMeta* owner = methodPair.second.first;
    MethodMeta* method = methodPair.second.second;

    string output = writeMethod(methodPair, meta, immediateProtocols, true);

    if (output.size()) {
//       out << "  // compoundInstanceMethods\n";

//      if (meta->nameExistsInSuperclass(method->jsName, Method)) {
//        cerr << "Skipping method that exists as method in " << meta->jsName << " superclass: `" << method->jsName << "`" << endl;
//        continue;
//      }
      
      if (meta->nameExistsInSuperclass(method->jsName, Property)) {
        cerr << "Skipping method that exists as property in " << meta->jsName << " superclass: `" << method->jsName << "`" << endl;
        continue;
      }
      
      if (ownStaticProperties.find(method->jsName) != ownStaticProperties.end()) {
        out << "  // dupe name w static property ";
      }
      if (ownInstanceProperties.find(method->jsName) != ownInstanceProperties.end()) {
        out << "  // dupe name w instance property ";
      }
      if (inheritedStaticMethods.find(method->jsName) != inheritedStaticMethods.end()) {
        out << "  // dupe name w inherited static method";
      }
      if (owner != meta) {
        out << "  // owner not meta";
      }
      
      out << "  " << output << endl;
    }
  }
  
  allNamespaces[meta->module->Name] = true;
  allClasses[meta->jsName] = true;
  
  out << "}" << endl << endl;
  
  if (containerName != metaName) {
    namespaceClasses[containerName].push_back(out.str());
    return;
  }

  _buffer << out.str();
}

string DefinitionWriter::writeProperty(PropertyMeta* propertyMeta, BaseClassMeta* owner, InterfaceMeta* target, CompoundMemberMap<PropertyMeta> baseClassProperties)
{
  bool optOutTypeChecking = false;
  auto result = baseClassProperties.find(propertyMeta->name);
  if (result != baseClassProperties.end()) {
    optOutTypeChecking = result->second.second->getter->signature[0] != propertyMeta->getter->signature[0];
  }
  
  if (propertyMeta->getUnavailableInSwift(owner)) {
    return "";
  }
  
  string output = writeProperty(propertyMeta, target, optOutTypeChecking);
  
  if (!output.empty()) {
    string out = "  ";
    
    if (clang::cast<clang::ObjCPropertyDecl>(propertyMeta->declaration)->isClassProperty()) {
      out += "static ";
    }
    
    return out + output + "\n";
  }
  
  return output;
}

void DefinitionWriter::getInheritedMembersRecursive(InterfaceMeta* interface,
                                                    CompoundMemberMap<MethodMeta>* staticMethods,
                                                    CompoundMemberMap<MethodMeta>* instanceMethods,
                                                    CompoundMemberMap<PropertyMeta>* staticProperties,
                                                    CompoundMemberMap<PropertyMeta>* instanceProperties)
{
  auto base = interface->base;
  if (!base) {
    return;
  }
  
  if (staticMethods) {
    for (MethodMeta* method : base->staticMethods) {
      if (staticMethods->find(method->name) == staticMethods->end()) {
        staticMethods->emplace(method->name, make_pair(base, method));
      }
    }
  }
  
  if (instanceMethods) {
    for (MethodMeta* method : base->instanceMethods) {
      if (instanceMethods->find(method->name) == instanceMethods->end()) {
        instanceMethods->emplace(method->name, make_pair(base, method));
      }
    }
  }
  
  if (staticProperties) {
    for (PropertyMeta* property : base->staticProperties) {
      if (staticProperties->find(property->name) == staticProperties->end()) {
        staticProperties->emplace(property->name, make_pair(base, property));
      }
    }
  }
  
  if (instanceProperties) {
    for (PropertyMeta* property : base->instanceProperties) {
      if (instanceProperties->find(property->name) == instanceProperties->end()) {
        instanceProperties->emplace(property->name, make_pair(base, property));
      }
    }
  }
  
  // accumulate...
  unordered_set<ProtocolMeta*> protocols;
  for (auto protocol : base->protocols) {
    getProtocolMembersRecursive(protocol, staticMethods, instanceMethods, staticProperties, instanceProperties, protocols);
  }
  
  getInheritedMembersRecursive(base, staticMethods, instanceMethods, staticProperties, instanceProperties);
}

void DefinitionWriter::getProtocolMembersRecursive(ProtocolMeta* protocolMeta,
                                                   CompoundMemberMap<MethodMeta>* staticMethods,
                                                   CompoundMemberMap<MethodMeta>* instanceMethods,
                                                   CompoundMemberMap<PropertyMeta>* staticProperties,
                                                   CompoundMemberMap<PropertyMeta>* instanceProperties,
                                                   unordered_set<ProtocolMeta*>& visitedProtocols)
{
  visitedProtocols.insert(protocolMeta);
  
  if (staticMethods) {
    for (MethodMeta* method : protocolMeta->staticMethods) {
      if (staticMethods->find(method->name) == staticMethods->end()) {
        staticMethods->emplace(method->name, make_pair(protocolMeta, method));
      }
    }
  }
  
  if (instanceMethods) {
    for (MethodMeta* method : protocolMeta->instanceMethods) {
      if (instanceMethods->find(method->name) == instanceMethods->end()) {
        instanceMethods->emplace(method->name, make_pair(protocolMeta, method));
      }
    }
  }
  
  if (staticProperties) {
    for (PropertyMeta* property : protocolMeta->staticProperties) {
      if (staticProperties->find(property->name) == staticProperties->end()) {
        staticProperties->emplace(property->name, make_pair(protocolMeta, property));
      }
    }
  }
  
  if (instanceProperties) {
    for (PropertyMeta* property : protocolMeta->instanceProperties) {
      if (instanceProperties->find(property->name) == instanceProperties->end()) {
        instanceProperties->emplace(property->name, make_pair(protocolMeta, property));
      }
    }
  }
  
  for (ProtocolMeta* protocol : protocolMeta->protocols) {
    getProtocolMembersRecursive(protocol, staticMethods, instanceMethods, staticProperties, instanceProperties, visitedProtocols);
  }
}

// MARK: - Visit Protocol

void DefinitionWriter::visit(ProtocolMeta* meta)
{
  string metaName = meta->jsName;
  
  allInterfaces[metaName] = true;
  
//  _buffer << "// protocol \n";
  _buffer << "interface " << metaName;

  map<string, PropertyMeta*> conformedProtocolsProperties;
  map<string, MethodMeta*> conformedProtocolsMethods;

//  if (meta->protocols.size()) {
//    _buffer << " extends ";
//    for (size_t i = 0; i < meta->protocols.size(); i++) {
//      transform(
//                     meta->protocols[i]->instanceProperties.begin(),
//                     meta->protocols[i]->instanceProperties.end(),
//                     inserter(conformedProtocolsProperties,
//                                   conformedProtocolsProperties.end()
//                                   ),
//                     []
//                     (PropertyMeta* propertyMeta) {
//        return make_pair(propertyMeta->name, propertyMeta);
//      });
//      transform(
//                     meta->protocols[i]->instanceMethods.begin(),
//                     meta->protocols[i]->instanceMethods.end(),
//                     inserter(conformedProtocolsMethods,
//                                   conformedProtocolsMethods.end()
//                                   ),
//                     []
//                     (MethodMeta* methodMeta) {
//        return make_pair(methodMeta->name, methodMeta);
//      });
//
//      _buffer << meta->protocols[i]->jsName;
//      if (i < meta->protocols.size() - 1) {
//        _buffer << ", ";
//      }
//    }
//  }
  
  _buffer << " {" << endl;
  
  for (PropertyMeta* property : meta->instanceProperties) {
    bool optOutTypeChecking = conformedProtocolsProperties.find(property->name) != conformedProtocolsProperties.end();
    
      // _buffer << "  // instance property\n";
    _buffer << "  " << writeProperty(property, meta, optOutTypeChecking) << endl;
  }
  
  for (MethodMeta* method : meta->instanceMethods) {
    if (method->getFlags(MethodIsInitializer)) {
      continue;
    }
    if (hiddenMethods.find(method->jsName) != hiddenMethods.end() ) {
      continue;
    }
    
    // Don't write methods that exist as props on class we are extending (for getters)
    if (conformedProtocolsMethods.find(method->name) != conformedProtocolsMethods.end()) {
      continue;
    }
    
    if (conformedProtocolsProperties.find(method->jsName) != conformedProtocolsProperties.end()) {
      continue;
    }
    
    string output = writeMethod(method, meta);
    
    if (output.size()) {
//       _buffer << "  // instance method\n";
      _buffer << "  " << output << endl;
    }
  }
  
  _buffer << "}" << endl << endl;
}

void getClosedGenericsIfAny(Type& type, vector<Type*>& params)
{
  if (type.is(TypeInterface)) {
    const InterfaceType& interfaceType = type.as<InterfaceType>();
    for (size_t i = 0; i < interfaceType.typeArguments.size(); i++) {
      getClosedGenericsIfAny(*interfaceType.typeArguments[i], params);
    }
  } else if (type.is(TypeTypeArgument)) {
    TypeArgumentType* typeArg = &type.as<TypeArgumentType>();
    if (typeArg->visit(NameRetrieverVisitor::instanceTs) != "") {
      if (find(params.begin(), params.end(), typeArg) == params.end()) {
        params.push_back(typeArg);
      }
    }
  }
}

string DefinitionWriter::getTypeString(clang::ASTContext &Ctx, clang::Decl::ObjCDeclQualifier Quals, clang::QualType qualType, const Type& type, const bool isFuncParam) {
  clang::QualType pointerType = Ctx.getUnqualifiedObjCPointerType(qualType);
  
  if (pointerType.getAsString().find("instancetype ", 0) != string::npos) {
    return "Self";
  }
  
  string formattedOut = tsifyType(type);

  return formattedOut;
}

bool DefinitionWriter::getTypeOptional(clang::ASTContext &Ctx, clang::Decl::ObjCDeclQualifier Quals, clang::QualType qualType, const Type& type, const bool isFuncParam) {
  bool isOptional = false;
  
  string formattedOut = tsifyType(type);
  
  if (Quals & clang::Decl::ObjCDeclQualifier::OBJC_TQ_CSNullability) {
    if (auto nullability = clang::AttributedType::stripOuterNullability(qualType)) {
      // TODO: dont use string comparison
      if (getNullabilitySpelling(*nullability, true).str() == "nullable") {
        isOptional = true;
      }
    }
  }
  
  if (qualType.getAsString().find("_Nullable") != string::npos) {
    // in some cases this is needed - like tabGroup in NSWindow
    // in theory these should show up in the attrs, but don't seem to
    isOptional = true;
  }
  
  return isOptional;
}

string DefinitionWriter::getInstanceParamsStr(MethodMeta* method, BaseClassMeta* owner) {
  vector<string> argumentLabels = method->argLabels;
  
  string output = "";
  const clang::ObjCMethodDecl& methodDecl = *clang::dyn_cast<clang::ObjCMethodDecl>(method->declaration);
  
  output += "(";
  
  const auto parameters = methodDecl.parameters();
  size_t lastParamIndex = method->getFlags(::Meta::MetaFlags::MethodHasErrorOutParameter) ? (parameters.size() - 1) : parameters.size();
  
  int numEmptyLabels = 0;
  int numLabels = argumentLabels.size();
  int numUnLabeledArgs = lastParamIndex - numLabels;
  bool hasOptional = false;
  map<string, int> usedNames = {};

  for (size_t i = 0; i < lastParamIndex; i++) {
    const clang::ParmVarDecl parmVar = *parameters[i];
    const clang::Decl::ObjCDeclQualifier qualifier = parmVar.getObjCDeclQualifier();
    clang::QualType qualType = parmVar.getType();
    
    string paramLabel = "";
    string paramName = "";
    
    if (numUnLabeledArgs > numEmptyLabels) {
      paramLabel = "_";
      numEmptyLabels++;
      
      if (numLabels > 0) {
        if (i < argumentLabels.size()) {
          paramName = argumentLabels[i];
        }
        else if (i < parameters.size()) {
          paramName = parameters[i]->getNameAsString();
        }
      }
    }
    else {
      auto idxToLookForName = i - numEmptyLabels;
      
      if (idxToLookForName < argumentLabels.size()) {
        paramName = argumentLabels[idxToLookForName];
      }
      
      paramLabel = paramName;
    }
    
    string pointerTypeStr = parmVar.getNameAsString();
    
//    if (pointerTypeStr.rfind("__kindof ", 0) == 0)
//    {
//      output += "typeof ";
//    }

    string finalName = sanitizeParameterName(paramLabel);
    
    usedNames[finalName]++;

    if (usedNames[finalName] > 1) {
      finalName += to_string(usedNames[finalName]);
    }
    
    output += finalName;
    
    bool isOptional = getTypeOptional(parmVar.getASTContext(), qualifier, qualType, *method->signature[i+1], true);
    
    if (isOptional || hasOptional) {
      output += "?";
      hasOptional = true;
    }
    
    output += ": ";
    
    // Param return type
    const Type* pType = method->signature[i+1];

    string returnType = tsifyType(*pType);
    returnType = jsifyTypeName(returnType);
    
    // TODO: fix these properly
    // hint: need to pass qualType into tsifyType like in Vue formatType
    // and/or merge those two fns (being careful to note where vue prop types
    // vs ts native types [String vs string] are written)
    
    // Something with formatTypeId here
    VueComponentFormatter::current.findAndReplaceIn2(returnType, ",id", ", any");
    
    // Could be done better, but need to be careful to only do this for params,
    // any maybe not for NSDictionary subclasses
    VueComponentFormatter::current.findAndReplaceIn2(returnType, "NSDictionary<", "Map<");
    
    if (returnType == "Dictionary" || returnType == "NSDictionary" || returnType == "NSMutableDictionary") {
      // sometimes shows up by itself without <generic> params
      returnType = "Map<any, any>";
    }
    if (returnType == "Array" || returnType == "NSArray" || returnType == "NSMutableArray") {
      // sometimes shows up by itself without <generic> params
      returnType = "any[]";
    }
    else if (returnType == "NSCache") {
      // this has something to do with the id type
      returnType = "NSCache<any, any>";
    }
    else if (returnType == "NSMeasurement") {
      returnType = "NSMeasurement<UnitType>";
    }
//    else if (returnType == "dtdKind") {
//      returnType = "XMLDTDNode.DTDKind";
//    }
    else if (returnType == "NSFetchRequest") {
      returnType = "NSFetchRequest<any>";
    }

    VueComponentFormatter::current.findAndReplaceIn2(returnType, "<NSCopying>", "");
//    VueComponentFormatter::current.findAndReplaceIn2(returnType, "dtdKind", "XMLDTDNode.DTDKind");
    
    output += sanitizeParameterName(returnType);

    if (i < lastParamIndex - 1) {
      output += ", ";
    }
  }
  
  output += ")";
  
  return output;
}

// MARK: - Write Method

string DefinitionWriter::writeMethod(MethodMeta* method, BaseClassMeta* owner, bool canUseThisType)
{
  const clang::ObjCMethodDecl& methodDecl = *clang::dyn_cast<clang::ObjCMethodDecl>(method->declaration);
  auto parameters = methodDecl.parameters();
  bool hasNonArrayNonObjectGeneric = false;
  vector<string> parameterNames;
  vector<Type*> paramsGenerics;
  
  vector<string> ownerGenerics;
  if (owner->is(Interface)) {
    ownerGenerics = getTypeParameterNames(clang::cast<clang::ObjCInterfaceDecl>(static_cast<const InterfaceMeta*>(owner)->declaration));
  }
  
  transform(parameters.begin(), parameters.end(), back_inserter(parameterNames), [](clang::ParmVarDecl* param) {
    return param->getNameAsString();
  });

  for (size_t i = 0; i < parameterNames.size(); i++) {
    if (hasNonArrayNonObjectGeneric) {
      break;
    }
    
    getClosedGenericsIfAny(*method->signature[i+1], paramsGenerics);
    
    if (method->signature[i+1]->visit(NameRetrieverVisitor::instanceTs) != "") {
      string paramType = VueComponentFormatter::current.formatType(*method->signature[i+1], parameters[i]->getType());

      if (paramType.substr(0, 4) == "Map<") {
        continue;
      }
      
      if (paramType == "JSManagedValue" || paramType == "JSValue") {
        hasNonArrayNonObjectGeneric = true;
      }
      
      if (paramType.find("<") != string::npos &&
          paramType.find("NSArray") == string::npos &&
          paramType.find("NSDictionary") == string::npos)
      {
        hasNonArrayNonObjectGeneric = true;
      }
    }
    
    for (size_t n = 0; n < parameterNames.size(); n++) {
      if (parameterNames[i] == parameterNames[n] && i != n) {
        parameterNames[n] += to_string(n);
      }
    }
  }
  
  if (!paramsGenerics.empty()) {
    for (size_t i = 0; i < paramsGenerics.size(); i++) {
      TypeArgumentType* typeArg = &paramsGenerics[i]->as<TypeArgumentType>();
      string name = typeArg->visit(NameRetrieverVisitor::instanceTs);
      
      if (find(ownerGenerics.begin(), ownerGenerics.end(), name) == ownerGenerics.end())
      {
        paramsGenerics.erase(paramsGenerics.begin() + i);
        i--;
      }
    }
  }
  
  ostringstream output;
  
  const Type* retType = method->signature[0];
  string returnType = computeMethodReturnType(retType, owner, canUseThisType);

  // For some reason, has different params than the method it is overriding
  if (owner->jsName == "NSMenuItemCell") {
    if (method->builtName() == "drawImageWithFrameInView" || method->builtName() == "drawTitleWithFrameInView") {
      output << "// ";
    }
  }

  if (method->getFlags(MethodIsInitializer)) {
    // TODO: Fix string comparison, use hasClosedGenerics() instead
    if (returnType.find("<") != string::npos) {
      return output.str();
    }
    
    output << "static ";
  }

//  string methodName = method->builtName();
//
//  if (methodName.empty()) {
//    cerr << "Falling back to " << method->jsName << " (" << method->name << ")" << endl;
    output << method->jsName;
//  }
//  else {
//    output << methodName;
//  }
  
  if (!methodDecl.isInstanceMethod() && owner->is(MetaType::Interface)) {
    if ((retType->is(TypeInstancetype) || retType->hasClosedGenerics())) {
      auto decl = clang::cast<clang::ObjCInterfaceDecl>(static_cast<const InterfaceMeta*>(owner)->declaration);
      output << getTypeParametersStringOrEmpty(decl);
    } else if (!paramsGenerics.empty()) {
      output << "<";
      for (size_t i = 0; i < paramsGenerics.size(); i++) {
        auto name = paramsGenerics[i]->visit(NameRetrieverVisitor::instanceTs);
        output << name;
        if (i < paramsGenerics.size() - 1) {
          output << ", ";
        }
      }
      output << ">";
    }
  }
  
  string paramsString = getInstanceParamsStr(method, owner);
  output << paramsString << ": ";
  
  // Method return type
  
  if (returnType == "NSFetchRequest") {
    returnType = "NSFetchRequest<any>";
  }
    
  output << returnType;

//  if (
//      (owner->type == MetaType::Protocol &&
//       methodDecl.getImplementationControl() == clang::ObjCMethodDecl::ImplementationControl::Optional)
//      ||
//      (owner->is(MetaType::Protocol) && method->getFlags(MethodIsInitializer))
//     ) {
//    output << "?";
//  }
  
  output << ";";
  
  return output.str();
}

string DefinitionWriter::writeMethod(CompoundMemberMap<MethodMeta>::value_type& methodPair, BaseClassMeta* owner, const unordered_set<ProtocolMeta*>& protocols, bool canUseThisType)
{
  
  BaseClassMeta* memberOwner = methodPair.second.first;
  MethodMeta* method = methodPair.second.second;

  if (method->getUnavailableInSwift(owner)) {
    return string();
  }
  
  if (hiddenMethods.find(method->jsName) != hiddenMethods.end()) {
    return string();
  }
  
  bool isOwnMethod = memberOwner == owner;
  bool implementsProtocol = protocols.find(static_cast<ProtocolMeta*>(memberOwner)) != protocols.end();
  bool returnsInstanceType = method->signature[0]->is(TypeInstancetype);
  
  ostringstream output;

//  if (method->jsName == "isEnabled") {
//    output << "// @ts-ignore \n  ";
//  }

  if (isOwnMethod || implementsProtocol || returnsInstanceType) {
    output << writeMethod(method, owner, canUseThisType);
  }
  
  return output.str();
}

// MARK: - Write Property

string DefinitionWriter::writeProperty(PropertyMeta* meta, BaseClassMeta* owner, bool optOutTypeChecking)
{
  ostringstream output;
  
  if (hiddenMethods.find(meta->jsName) != hiddenMethods.end()) {
    return string();
  }
  
  if (meta->getUnavailableInSwift(owner)) {
    output << "  // unavailableInSwift ";
  }
  
  // for isEnabled b/c lazy
//  if (meta->jsName == "isEnabled") {
//    output << "// @ts-ignore \n  ";
//  }
  
  if (meta->getter && !meta->getter->jsName.empty()) {
    output << meta->getter->jsName;
  }
  else {
    output << meta->jsName;
  }

  if (owner->is(MetaType::Protocol) && clang::dyn_cast<clang::ObjCPropertyDecl>(meta->declaration)->getPropertyImplementation() == clang::ObjCPropertyDecl::PropertyControl::Optional) {
    output << "?";
  }
  
  output << ": ";
  
  // Property return type
  
  const Type* retType = meta->getter->signature[0];
  string returnType = computeMethodReturnType(retType, owner);

  output << jsifyTypeName(returnType);

  output << ";";
  
  if (meta->setter && !meta->setter->jsName.empty()) {
    string setterName = meta->setter->name;
    setterName.pop_back();
    
    // Write setter too
    output << endl;
    output << "  " << setterName << "(_: " + returnType + ");";
  }
  
  return output.str();
}

void DefinitionWriter::visit(CategoryMeta* meta)
{
  allNamespaces[meta->module->Name] = true;
}

void DefinitionWriter::visit(FunctionMeta* meta)
{
  allNamespaces[meta->module->Name] = true;

  const clang::FunctionDecl& functionDecl = *clang::dyn_cast<clang::FunctionDecl>(meta->declaration);
  ostringstream params;

  for (size_t i = 1; i < meta->signature.size(); i++) {
    string name = sanitizeParameterName(functionDecl.getParamDecl(i - 1)->getNameAsString());
    string tsified = tsifyType(*meta->signature[i], true);
    string renamed = renamedName(tsified);
    string paramTypeStr = jsifyTypeName(renamed);

    params << (name.size() ? name : "p" + to_string(i)) << ": " << paramTypeStr;

    if (i < meta->signature.size() - 1) {
      params << ", ";
    }
  }

  // Commented out because we need to property build params (enums with namespaces)

  _buffer << "// export function ";
  _buffer << meta->jsName;
  _buffer << "(" << params.str() << "): ";

  string returnName;

  if (meta->jsName == "UIApplicationMain" || meta->jsName == "NSApplicationMain" || meta->jsName == "dispatch_main") {
    returnName = "never";
  }
  else {
    returnName = tsifyType(*meta->signature[0]);
  }

  _buffer << returnName << ";\n\n";
}

map<string, vector<StructMeta*>> objcStructs;

void DefinitionWriter::visit(StructMeta* meta)
{
//  cout << "struct: " << meta->name << endl;

  allNamespaces[meta->module->Name] = true;
//  string metaName = meta->jsName;
//  TSComment comment = _docSet.getCommentFor(meta);
//  string members = writeMembers(meta->fields, comment.fields);
  objcStructs[meta->module->Name].push_back(meta);
//
//  if (!members.empty()) {
//    _buffer << "// struct \n";
//    _buffer << "// interface " << metaName << " {\n";
////    _buffer << members;
//    _buffer << "// }\n\n";
//  }
}

void DefinitionWriter::visit(UnionMeta* meta)
{
  string metaName = meta->jsName;
  TSComment comment = _docSet.getCommentFor(meta);
  string members = writeMembers(meta->fields, comment.fields);

  if (!members.empty()) {
    allNamespaces[meta->module->Name] = true;
    
    _buffer << "// union \n";
    _buffer << "interface " << metaName << " {\n";
    _buffer << members;
    _buffer << "}\n\n";
  }
}

string DefinitionWriter::writeMembers(const vector<RecordField>& fields, vector<TSComment> fieldsComments)
{
  string members;
  
  for (size_t i = 0; i < fields.size(); i++) {
    if (fields[i].name[0] != '_') {
      members += "  " + fields[i].name + ": " + tsifyType(*fields[i].encoding) + ";\n";
    }
  }
  
  return members;
}

// MARK: - Visit Enum
void DefinitionWriter::visit(EnumMeta* meta)
{
//  cout << "enum: " << meta->name << endl;

  string enumName;
  string enumContainer = "_container";
  string moduleName = renamedName(meta->module->Name);

  string fullEnumName = renamedName(meta->jsName);
  vector<string> enumTokens;
  StringUtils::split(fullEnumName, '.', back_inserter(enumTokens));
  
  if (enumTokens.size() == 1) {
    enumName = renamedName(enumTokens[0]);
  }
  else if (enumTokens.size() == 2) {
    enumContainer = renamedName(enumTokens[0]);
    enumName = renamedName(enumTokens[1]);
  }
  
  string renamedEnumContainer = renamedName(enumContainer);
  vector<string> renamedEnumContainerTokens;
  StringUtils::split(renamedEnumContainer, '.', back_inserter(renamedEnumContainerTokens));

  if (renamedEnumContainerTokens.size() == 1) {
    enumContainer = renamedName(renamedEnumContainerTokens[0]);
  }
  else if (renamedEnumContainerTokens.size() == 2) {
    moduleName = renamedName(renamedEnumContainerTokens[0]);
    enumContainer = renamedName(renamedEnumContainerTokens[1]);
  }

  string renamedEnumName = renamedName(enumName);
  vector<string> renamedEnumNameTokens;
  StringUtils::split(renamedEnumName, '.', back_inserter(renamedEnumNameTokens));
  
  if (renamedEnumNameTokens.size() == 1) {
    enumName = renamedName(renamedEnumNameTokens[0]);
  }
  else if (renamedEnumNameTokens.size() == 2) {
    moduleName = renamedName(renamedEnumNameTokens[0]);
    enumContainer = renamedName(renamedEnumNameTokens[1]);
  }

  allNamespaces[moduleName] = true;
  
  if (moduleName == enumContainer) {
    enumContainer = "_container";
  }
  else {
    // Add any enums from base classes
    if (moduleName == "NSCollectionViewFlowLayout") {
      vector<string> parentNameParts;
      StringUtils::split(meta->jsName, '.', back_inserter(parentNameParts));

      if (parentNameParts.size() > 1) {
        string parentName = parentNameParts[0];
        
        for (const auto& fooBar: namespaceEnums[parentName]["_container"]) {
          for (const auto& baz: fooBar.second) {
            if (namespaceEnums[moduleName][enumContainer][fooBar.first].empty()) {
              namespaceEnums[moduleName][enumContainer][fooBar.first].push_back(baz);
            }
          }
        }
      }
    }

    if (enumContainer != "_container") {
      if (namespaceEnums[moduleName][enumContainer][enumName].empty()) {
        namespaceEnums[enumContainer]["_container"][enumName].push_back(meta);
      }
    }
  }
  
  if (namespaceEnums[moduleName][enumContainer][enumName].empty()) {
    namespaceEnums[moduleName][enumContainer][enumName].push_back(meta);
  }
}

void DefinitionWriter::visit(VarMeta* meta)
{
  allNamespaces[meta->module->Name] = true;
  namespaceVars[meta->module->Name].push_back(meta);
}

string DefinitionWriter::writeFunctionProto(const vector<Type*>& signature)
{
  ostringstream output;
  output << "(";
  
  for (size_t i = 1; i < signature.size(); i++) {
    output << "p" << i << ": " << tsifyType(*signature[i]);
    if (i < signature.size() - 1) {
      output << ", ";
    }
  }
  
  output << ") => " << tsifyType(*signature[0]);
  
  return output.str();
}

void DefinitionWriter::visit(MethodMeta* meta)
{
  allNamespaces[meta->module->Name] = true;
}

void DefinitionWriter::visit(PropertyMeta* meta)
{
  allNamespaces[meta->module->Name] = true;
  }

void DefinitionWriter::visit(EnumConstantMeta* meta)
{
  allNamespaces[meta->module->Name] = true;
}

// MARK: - TSify Type

string DefinitionWriter::tsifyType(const Type& type, const bool isFuncParam)
{
  string typeStr;
  string enumModuleName;
  
  switch (type.getType()) {
    case TypeVoid:
      typeStr = "void";
      break;
    case TypeBool:
      typeStr = "boolean";
      break;
    case TypeSignedChar:
    case TypeUnsignedChar:
    case TypeShort:
    case TypeUShort:
    case TypeInt:
    case TypeUInt:
    case TypeLong:
    case TypeULong:
    case TypeLongLong:
    case TypeULongLong:
    case TypeFloat:
    case TypeDouble:
      typeStr = "number";
      break;
    case TypeUnichar:
    case TypeSelector:
      typeStr = "string";
      break;
    case TypeCString: {
      string res = "string";
      if (isFuncParam) {
        Type typeVoid(TypeVoid);
        res += " | " + Type::nameForJSExport(Type::lookupApiNotes(tsifyType(::Meta::PointerType(&typeVoid), isFuncParam)));
      }
      typeStr = res;
      break;
    }
    case TypeProtocol:
      typeStr = "any /* Protocol */";
      break;
    case TypeClass:
      typeStr = "typeof NSObject";
      break;
    case TypeId: {
      const IdType& idType = type.as<IdType>();
      if (idType.protocols.size() == 1 && idType.protocols[0]->jsName != "NSCopying") {
          typeStr = idType.protocols[0]->jsName;
      }
      typeStr = "any";
      break;
    }
    case TypeConstantArray:
    case TypeExtVector:
      //        typeStr = "interop.Reference<" + tsifyType(*type.as<ConstantArrayType>().innerType) + ">";
      typeStr = Type::nameForJSExport(Type::lookupApiNotes(tsifyType(*type.as<ConstantArrayType>().innerType)));
      break;
    case TypeIncompleteArray:
      //        typeStr = "interop.Reference<" + tsifyType(*type.as<IncompleteArrayType>().innerType) + ">";
      typeStr = Type::nameForJSExport(Type::lookupApiNotes(tsifyType(*type.as<IncompleteArrayType>().innerType)));
      break;
    case TypePointer: {
      const PointerType& pointerType = type.as<PointerType>();
      typeStr = (pointerType.innerType->is(TypeVoid)) ? "any" : Type::nameForJSExport(Type::lookupApiNotes(tsifyType(*pointerType.innerType)));
      break;
    }
    case TypeBlock:
      return Type::nameForJSExport(Type::lookupApiNotes(writeFunctionProto(type.as<BlockType>().signature)));
    case TypeFunctionPointer:
      return Type::nameForJSExport(Type::lookupApiNotes(writeFunctionProto(type.as<FunctionPointerType>().signature)));
    case TypeInterface:
    case TypeBridgedInterface: {
      if (type.is(TypeType::TypeBridgedInterface) && type.as<BridgedInterfaceType>().isId()) {
        typeStr = Type::nameForJSExport(Type::lookupApiNotes(tsifyType(IdType())));
        break;
      }

      const InterfaceMeta& interface = type.is(TypeType::TypeInterface) ? *type.as<InterfaceType>().interface : *type.as<BridgedInterfaceType>().bridgedInterface;
      const InterfaceType& interfaceType = type.as<InterfaceType>();

      if (interface.name == "NSNumber") {
        typeStr = "number";
        break;
      }
      else if (interface.name == "NSString") {
        typeStr = "string";
        break;
      }
      else if (interface.name == "NSDate") {
        typeStr = "Date";
        break;
      }

      ostringstream output;
      bool hasClosed = type.hasClosedGenerics();
      string interfaceName = interface.jsName;
      bool isDictionary = interface.name == "NSDictionary" || interface.name == "NSMutableDictionary";
      bool isArray = interface.name == "NSArray" || interface.name == "NSMutableArray";

      if (isDictionary) {
        interfaceName = "Map";
      }

      if (isArray) {
        if (hasClosed) {
          string arrayType = tsifyType(*interfaceType.typeArguments[0]);
          output << renamedName(arrayType) << "[]";
        }
        else {
          output << "any[]";
        }
      }
      else if (hasClosed) {
        output << interfaceName;
        if (interfaceType.typeArguments.size() == 2) {
          output << "<";
          output << tsifyType(*interfaceType.typeArguments[0]);
          output << ", ";
          output << tsifyType(*interfaceType.typeArguments[1]);
          output << ">";
        }
        else {
          output << "<any>";
        }
      }
      else {
        output << interfaceName;
        
        // This also translates CFArray to NSArray<any>
        if (auto typeParamList = clang::dyn_cast<clang::ObjCInterfaceDecl>(interface.declaration)->getTypeParamListAsWritten()) {
          output << "<";
          for (size_t i = 0; i < typeParamList->size(); i++) {
            output << "any";
            if (i < typeParamList->size() - 1) {
              output << ", ";
            }
          }
          output << ">";
        }
      }

      typeStr = output.str();

      break;
    }
    case TypeStruct: {
      const StructType& structType = type.as<StructType>();
      typeStr = structType.structMeta->jsName;
      break;
    }
    case TypeUnion:
      typeStr = (*type.as<UnionType>().unionMeta).jsName;
      break;
    case TypeAnonymousStruct:
    case TypeAnonymousUnion: {
      ostringstream output;
      output << "{ ";
      const vector<RecordField>& fields = type.as<AnonymousStructType>().fields;
      for (auto& field : fields) {
        output << Type::nameForJSExport(Type::lookupApiNotes(field.name)) << ": " << Type::nameForJSExport(Type::lookupApiNotes(tsifyType(*field.encoding))) << "; ";
      }
      output << "}";
      typeStr = output.str();
      break;
    }
    case TypeEnum: {
      const EnumType& enumType = type.as<EnumType>();
      enumModuleName = renamedName(enumType.enumMeta->module->Name);
      typeStr = enumType.enumMeta->jsName;
      break;
    }
    case TypeTypeArgument:
      typeStr = Type::nameForJSExport(Type::lookupApiNotes(type.as<TypeArgumentType>().name));
      break;
    case TypeVaList:
    case TypeInstancetype:
    default:
      break;
  }
  
  string typeName = renamedName(typeStr);
  string containerName = "_container";
  string moduleName = "_container";
  
  vector<string> typeNameTokens;
  StringUtils::split(typeStr, '.', back_inserter(typeNameTokens));
  
  if (typeNameTokens.size() == 1) {
    typeName = renamedName(typeNameTokens[0]);
  }
  else if (typeNameTokens.size() == 2) {
    containerName = renamedName(typeNameTokens[0]);
    typeName = renamedName(typeNameTokens[1]);
  }
  
  string renamedTypeContainer = renamedName(containerName);
  vector<string> renamedTypeContainerTokens;
  StringUtils::split(renamedTypeContainer, '.', back_inserter(renamedTypeContainerTokens));
  
  if (renamedTypeContainerTokens.size() == 1) {
    containerName = renamedName(renamedTypeContainerTokens[0]);
  }
  else if (renamedTypeContainerTokens.size() == 2) {
    moduleName = renamedName(renamedTypeContainerTokens[0]);
    containerName = renamedName(renamedTypeContainerTokens[1]);
  }
  
  string renamedTypeName = renamedName(typeName);
  vector<string> renamedTypeNameTokens;
  StringUtils::split(renamedTypeName, '.', back_inserter(renamedTypeNameTokens));
  
  if (renamedTypeNameTokens.size() == 1) {
    typeName = renamedName(renamedTypeNameTokens[0]);
  }
  else if (renamedTypeNameTokens.size() == 2) {
    moduleName = renamedName(renamedTypeNameTokens[0]);
    containerName = renamedName(renamedTypeNameTokens[1]);
  }
  
  if (moduleName == "_container" && containerName == "_container") {
    if (enumModuleName.empty()) {
      return typeName;
    }
    else {
      return enumModuleName + "." + typeName;
    }
  }
  
  if (moduleName == "_container") {
    if (!enumModuleName.empty() &&
        enumModuleName != containerName &&
        enumModuleName != containerName + "." + typeName) {
      return enumModuleName + "." + containerName + "." + typeName;
    }
    else {
      return containerName + "." + typeName;
    }
  }
  
  if (containerName == "_container") {
    return moduleName + "." + typeName;
  }
  
  return moduleName + "." + containerName + "." + typeName;
}

string DefinitionWriter::computeMethodReturnType(const Type* retType, const BaseClassMeta* owner, bool instanceMember)
{
  ostringstream output;
  
  if (retType->is(TypeInstancetype)) {
    output << owner->jsName;
    
    if (owner->is(MetaType::Interface)) {
      output << getTypeParametersStringOrEmpty(clang::cast<clang::ObjCInterfaceDecl>(static_cast<const InterfaceMeta*>(owner)->declaration));
    }
  }
  else {
    string typeStr = tsifyType(*retType);
    
    // Add prefix for types that are in same namespace as this class name
    //
    // e.g. NSTypesetterControlCharacterAction -> NSTypesetter.NSTypesetterControlCharacterAction
    //
    if (namespaceEnums[owner->jsName]["_container"][typeStr].size()) {
      typeStr = owner->jsName + "." + typeStr;
    }
    
    output << typeStr;
  }
  
  string out = output.str();
  
  if (out.empty()) {
    out = "void";
  }
  
  if (out == "id") {
    out = "any";
  }
  
  return out;
}

string DefinitionWriter::write()
{
  _buffer.clear();
  _importedModules.clear();
  
  // Decorate bridged classes with POJO of enum values,
  // so that they are available at runtime and don't get
  // clobbered when javascriptcore bridges these classes.
  for (::Meta::Meta* meta : _module.second) {
    if (meta->is(Enum)) {
      meta->visit(this);
    }
  }
  
  _buffer.clear();

  for (::Meta::Meta* meta : _module.second) {
    if (meta->is(Enum)) {
      meta->visit(this);
    }
  }
  
  for (::Meta::Meta* meta : _module.second) {
    if (!meta->is(Enum)) {
      if (JSExportDefinitionWriter::hiddenClasses.find(meta->jsName) == JSExportDefinitionWriter::hiddenClasses.end()) {
        meta->visit(this);
      }
    }
  }
  
  return _buffer.str();
}

// MARK: - Write Exports

string DefinitionWriter::writeExports() {
  ostringstream output;
  map<string, bool> writtenExports = {};

  output << "export {\n";
  
  size_t i = 1;
  
  for (const auto& namespaceRecord : allNamespaces) {
    i++;
    
    string namespaceName = renamedName(namespaceRecord.first);
    
    vector<string> namespaceNameTokens;
    StringUtils::split(namespaceName, '.', back_inserter(namespaceNameTokens));
    
    if (namespaceNameTokens.size() == 2) {
      namespaceName = namespaceNameTokens[0];
    }
    
    bool hasDecls = namespaceEnums[namespaceName].size()
      || namespaceEnums[namespaceName].size()
      || namespaceVars[namespaceName].size()
      || namespaceTypealiases[namespaceName].size()
      || allClasses[namespaceName]
      || allInterfaces[namespaceName];
    
    if (!hasDecls ||
        namespaceName == "XMLNode.Options" ||
        namespaceRecord.first == "NSNetService" ||
        namespaceRecord.first == "runtime") {
      continue;
    }
    
    if (namespaceName.substr(0, 7) == "_global") {
      continue;
    }
    
    if (namespaceName == "Set" ||
        namespaceName == "Array" ||
        namespaceName == "String" ||
        namespaceName == "NSObject") {
      continue;
    }

    if (writtenExports[namespaceName]) {
      continue;
    }
    
    writtenExports[namespaceName] = true;
    
    if (namespaceName == "Date" || namespaceName == "Error") {
      output << "  NS" << namespaceName << " as " << namespaceName;
    }
    else {
      output << "  " << namespaceName;
    }
    
    if (i < allNamespaces.size() - 2) {
      output << ",";
    }
    
    output << "\n";
  }
  
  output << "};\n";
  
  namespaceClasses.empty();
//  namespaceEnums.empty();
  namespaceVars.empty();
  namespaceTypealiases.empty();
//  allNamespaces.empty();
  allClasses.empty();
  allInterfaces.empty();
  
  namespaceClasses = {};
//  namespaceEnums = {};
  namespaceVars = {};
  namespaceTypealiases = {};
//  allNamespaces = {};
  allClasses = {};
  allInterfaces = {};
  
  return output.str();
}

string DefinitionWriter::writeNamespaces(bool writeToClasses)
{
  populateTypealiases();
  
  ostringstream output;

  map<string, bool> writtenEnums = {};
  map<string, bool> writtenNamespaces = {};

  for (const auto& namespaceRecord : allNamespaces) {
    string namespaceName = renamedName(namespaceRecord.first);

    vector<string> namespaceNameTokens;
    StringUtils::split(namespaceName, '.', back_inserter(namespaceNameTokens));
    
    if (namespaceNameTokens.size() == 2) {
      namespaceName = namespaceNameTokens[0];
    }

    bool hasDecls = namespaceEnums[namespaceName].size() ||
      namespaceClasses[namespaceName].size() ||
      namespaceVars[namespaceName].size() ||
      namespaceTypealiases[namespaceName].size();
    
    if (!hasDecls || namespaceRecord.first == "NSNetService") {
      continue;
    }
    
    if (writtenNamespaces[namespaceName]) {
      continue;
    }
    
    writtenNamespaces[namespaceName] = true;

    if (!writeToClasses) {
      if (ignoredNamespaces.find(namespaceName) != ignoredNamespaces.end()) {
        output << "// @ts-ignore\n";
      }

      output << "export namespace " << namespaceName << " {\n";
    }

    // MARK: - Namespace Classes

    if (!writeToClasses) {
      for (auto& namespaceClass : namespaceClasses[namespaceName]) {
        // Indent properly
        VueComponentFormatter::current.findAndReplaceIn2(namespaceClass, "\n", "\n  ");
        // Remove last two spaces
        namespaceClass.pop_back();
        namespaceClass.pop_back();
        
        // Write out string we collected earlier when visiting interface
        output << "  " << namespaceClass;
      }
    }
    
    // MARK: - Namespace Enums
    
    if (writeToClasses && namespaceEnums[namespaceName].size()) {
      output << "(globalThis as any)['" << namespaceName << "'] = (globalThis as any)['" << namespaceName << "'] || {};" << endl;
    }

    for (const auto& namespaceClass : namespaceEnums[namespaceName]) {
      string className = namespaceClass.first;
      bool isContainer = className == "_container";
      const string SP = isContainer ? "" : "  ";

      if (namespaceClass.second.size()) {
        if (!writeToClasses && !isContainer) {
          output << SP << "export namespace " << className << " {" << endl;
        }
      }
      
      for (const auto& namespaceContainer : namespaceClass.second) {
        string enumName = namespaceContainer.first;
        if (writtenEnums[namespaceName + "." + className + "." + enumName]) {
          continue;
        }
        
        writtenEnums[namespaceName + "." + className + "." + enumName] = true;

        if (!namespaceContainer.second.size()) {
          continue;
        }

        if (writeToClasses) {
          if (!isContainer) {
            output << "(globalThis as any)['" << namespaceName << "']";
            output << "['" << className << "']";
            output << " = (globalThis as any)";
            output << "['" << namespaceName << "']";
            output << "['" << className << "']";
            output << " || {};" << endl;
          }

          output << "(globalThis as any)['" << namespaceName << "']";
          if (!isContainer) {
            output << "['" << className << "']";
          }
          output << "['" << enumName << "']";

          output << " = (globalThis as any)";
          output << "['" << namespaceName << "']";

          if (!isContainer) {
            output << "['" << className << "']";
          }
          output << "['" << enumName << "']";
          output << " || {};" << endl;
        }

        for (const auto& namespaceEnum : namespaceContainer.second) {
          vector<EnumField>& fields = namespaceEnum->swiftNameFields.size() != 0 ? namespaceEnum->swiftNameFields : namespaceEnum->fullNameFields;

          if (writeToClasses) {
            output << "(globalThis as any)['" << namespaceName << "']";

            if (!isContainer) {
              output << "['" << className << "']";
            }

            output << "['" << enumName << "']";
            output << " = {\n";

            for (size_t i = 0; i < fields.size(); i++) {
              output << "  " +  fields[i].name + ": " + fields[i].value;
              if (i < fields.size() - 1) {
                output << ",";
              }
              output << "\n";
            }
            output << "};\n\n";
          }
          else {
            if (enumName == "dtdKind") {
              enumName = "DTDKind";
            }
            output << SP << "  export enum " << enumName << " {\n";

            for (size_t i = 0; i < fields.size(); i++) {
              output << SP << "    " +  fields[i].name + " = " + fields[i].value;

              if (i < fields.size() - 1) {
                output << ",";
              }

              output << "\n";
            }

            output << SP << "  }" << endl;

            if (isContainer) {
              output << endl;
            }
          }

          writtenEnums[namespaceName + "." + className + "." + enumName] = true;
        }
      }

      if (namespaceClass.second.size()) {
        if (!writeToClasses && !isContainer) {
          output << SP << "}" << endl << endl;
        }
      }
    }

    if (!writeToClasses) {
      output << "}" << endl;
    }

    // MARK: - Namespace Vars
    
//
//    map<string, bool> writtenVars = {};
//
//    for (const auto& namespaceVar : namespaceVars[namespaceName]) {
//      string varName = sanitizeParameterName(namespaceVar->jsName);
//
//      if (writtenVars[varName]) {
//        continue;
//      }
//
//      output << "//  export let " << varName << ": " << jsifyTypeName(tsifyType(*namespaceVar->signature)) << ";\n";
//
//      writtenVars[varName] = true;
//    }
    
    // MARK: - Namespace ObjC Structs
    
//    for (const auto& structMeta : objcStructs[namespaceName]) {
//    }
    
    
    // MARK: - Namespace Typealiases
    
//
//    for (const auto& typeAliases : namespaceTypealiases[namespaceName]) {
//      output << "  export type " << typeAliases.first << " = " << jsifyTypeName(typeAliases.second) <<  ";\n";
//    }
    
    output << "\n";
  }

  // MARK: - Global Structs
  
//
//  for (const auto& structClass : structsMeta["_global"]) {
//    string structClassName = structClass.first;
//
//    map<string, bool> previousNames = {};
//
//    auto values = structClass.second["values"].as<vector<string>>();
//
//    if (values.empty()) {
//      continue;
//    }
//
//    output << "export enum " << structClassName << " {\n";
//
//    for (const auto& value : values) {
//      if (previousNames[value]) {
//        continue;
//      }
//
//      previousNames[value] = true;
//
//      output << "  " << value << ",\n";
//    }
//
//    output << "}\n\n";
//
//    if (writeToClasses) {
//      output << "global['" << structClassName << "'] = " << structClassName << ";\n\n";
//    }
//  }
//
//  MARK: - Global Enums
  
//  for (const auto& namespaceEnum : namespaceEnums["_global"]) {
//    // Enum values
//    vector<EnumField>& fields = namespaceEnum->swiftNameFields.size() != 0 ? namespaceEnum->swiftNameFields : namespaceEnum->fullNameFields;
//    string enumName = namespaceEnum->jsName;
//
//    output << "export enum " << enumName << " {\n";
//
//    for (size_t i = 0; i < fields.size(); i++) {
//      output << "  " +  fields[i].name + " = " + fields[i].value;
//
//      if (i < fields.size() - 1) {
//        output << ",";
//      }
//
//      output << "\n";
//    }
//
//    output << "}\n\n";
//
//    if (writeToClasses) {
//      output << "global['" << enumName << "'] = " << enumName << ";\n\n";
//    }
//  }
//
//  MARK: - Global Vars
  
//  for (const auto& namespaceVar : namespaceVars["_global"]) {
//    string varName = sanitizeParameterName(namespaceVar->jsName);
//
//    output << "// export let " << varName << ": " << jsifyTypeName(tsifyType(*namespaceVar->signature)) << ";\n";
//
//    if (writeToClasses) {
//      output << "// global['" << varName << "'] = " << varName << ";\n\n";
//    }
//  }

  return output.str();
}

}
