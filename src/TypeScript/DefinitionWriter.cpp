#include "DefinitionWriter.h"
#include "Meta/Utils.h"
#include "Meta/NameRetrieverVisitor.h"
#include "Meta/MetaFactory.h"
#include "Meta/MetaData.h"
#include "Utils/StringUtils.h"
#include "JSExport/JSExportDefinitionWriter.h"
#include "JSExport/JSExportFormatter.h"
#include "Vue/VueComponentFormatter.h"
#include "JSExport/JSExportMeta.h"
#include "yaml-cpp/yaml.h"
#include <algorithm>
#include <clang/AST/DeclObjC.h>
#include <iterator>

namespace TypeScript {
using namespace Meta;
using namespace std;

static map<string, vector<EnumMeta*>> namespaceEnums = {};
static map<string, vector<VarMeta*>> namespaceVars = {};
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
  "countByEnumeratingWithStateObjectsCount",
  "create"
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
    { "dtdKind", "XMLDTDNode.DTDKind" },
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

static map<string, map<string, YAML::Node>> structsMeta = {};

void DefinitionWriter::populateStructsMeta()
{
  string structsLookupPath = dataRoot + "/structs.json";
  
  auto structsNode = YAML::LoadFile(structsLookupPath);
  
  for (YAML::const_iterator it = structsNode.begin(); it != structsNode.end(); ++it) {
    for (YAML::const_iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
      for (YAML::const_iterator it3 = it2->second.begin(); it3 != it2->second.end(); ++it3) {
        // NSApplication.ActivationOptions = { values, type }
        string moduleName = it2->first.as<string>();
        string className = it3->first.as<string>();
        structsMeta[MetaData::renamedName(moduleName)][MetaData::renamedName(moduleName + "." + className)] = it3->second;
      }
    }
  }
}

void populateTypealiases()
{
  string aliasesPath = dataRoot + "/aliases.json";
  
  auto aliasNode = YAML::LoadFile(aliasesPath);
  
  for (YAML::const_iterator it = aliasNode.begin(); it != aliasNode.end(); ++it) {
    string moduleName = it->first.as<string>();
    
    for (YAML::const_iterator it2 = it->second.begin(); it2 != it->second.end(); ++it2) {
      string f = it2->first.as<string>();
      string s = it2->second.as<string>();
      
      namespaceTypealiases[MetaData::renamedName(moduleName)][f] = s;
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
  
  // TODO: Need to separate into ES6 modules to prevent overlaps like this
  if (meta->jsName == "Port" && meta->module->getTopLevelModule()->Name == "AVFoundation") {
    return;
  }
  
  string metaName = meta->jsName;
  string parametersString = getTypeParametersStringOrEmpty(clang::cast<clang::ObjCInterfaceDecl>(meta->declaration));
  
  _buffer << "// interface\n";
  _buffer << "class " << metaName << parametersString;

  if (meta->base != nullptr) {
    _buffer << " extends " << meta->base->jsName << getTypeArgumentsStringOrEmpty(clang::cast<clang::ObjCInterfaceDecl>(meta->declaration)->getSuperClassType());
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
    _buffer << " implements ";
    
    for (size_t i = 0; i < metaProtocols.size(); i++) {
      string protoName = metaProtocols[i]->jsName;
      
      getProtocolMembersRecursive(metaProtocols[i], &compoundStaticMethods, &compoundInstanceMethods, &protocolInheritedStaticProperties, &protocolInheritedInstanceProperties, protocols);
      
      _buffer << protoName;
      
      if (i < metaProtocols.size() - 1) {
        _buffer << ", ";
      }
    }
  }
  
  _buffer << " {" << endl;

  unordered_set<ProtocolMeta*> immediateProtocols;
  
  for (auto protocol : protocols) {
    if (inheritedProtocols.find(protocol) == inheritedProtocols.end()) {
      immediateProtocols.insert(protocol);
    }
  }
  
  for (auto& methodPair : compoundStaticMethods) {
    string output = writeMethod(methodPair, meta, immediateProtocols);

    if (output.size()) {
      _buffer << "  // compoundStaticMethods\n";

      if (inheritedStaticMethods.find(methodPair.first) != inheritedStaticMethods.end()) {
        _buffer << "  //";
      }

      _buffer << "  static " << output << endl;
    }
  }
  
  for (auto& propertyPair : ownInstanceProperties) {
    BaseClassMeta* owner = propertyPair.second.first;
    PropertyMeta* propertyMeta = propertyPair.second.second;

     _buffer << "  // ownInstanceProperties\n";

    // if this (selectedCell) ownInstanceProperties exists in parent class's methods (selectedCell())
    if (protocolInheritedStaticProperties.find(propertyMeta->name) != protocolInheritedStaticProperties.end()) {
      _buffer << "  // ";
    }
    if (ownStaticProperties.find(propertyMeta->name) != ownStaticProperties.end()) {
      _buffer << "  // ";
    }
    if (baseClassStaticProperties.find(propertyMeta->name) != baseClassStaticProperties.end()) {
      _buffer << "  // ";
    }
    if (inheritedStaticMethods.find(propertyMeta->name) != inheritedStaticMethods.end()) {
      _buffer << "  // ";
    }
    if (owner != meta) {
      _buffer << "  // ";
    }

    this->writeProperty(propertyMeta, owner, meta, baseClassInstanceProperties);
  }
  
  for (auto& propertyPair : ownStaticProperties) {
    BaseClassMeta* owner = propertyPair.second.first;
    PropertyMeta* propertyMeta = propertyPair.second.second;
    
   _buffer << "  // ownStaticProperties\n";

    if (owner != meta) {
      _buffer << "  // ";
    }
    
    this->writeProperty(propertyMeta, owner, meta, baseClassInstanceProperties);
  }
  
  for (auto& propertyPair : protocolInheritedInstanceProperties) {
    BaseClassMeta* owner = propertyPair.second.first;
    PropertyMeta* propertyMeta = propertyPair.second.second;
    
     _buffer << "  // protocolInheritedInstanceProperties\n";

    bool isDuplicated = ownInstanceProperties.find(propertyMeta->name) != ownInstanceProperties.end();
    
    if (immediateProtocols.find(reinterpret_cast<ProtocolMeta*>(owner)) == immediateProtocols.end() || isDuplicated) {
      _buffer << "  // ";
    }
    
    this->writeProperty(propertyMeta, owner, meta, baseClassInstanceProperties);
  }
  
  for (auto& propertyPair : protocolInheritedStaticProperties) {
    BaseClassMeta* owner = propertyPair.second.first;
    PropertyMeta* propertyMeta = propertyPair.second.second;

    _buffer << "  // protocolInheritedStaticProperties\n";
    this->writeProperty(propertyMeta, owner, meta, baseClassStaticProperties);
  }
  
//  auto objectAtIndexedSubscript = compoundInstanceMethods.find("objectAtIndexedSubscript");
//  if (objectAtIndexedSubscript != compoundInstanceMethods.end()) {
//    const Type* retType = objectAtIndexedSubscript->second.second->signature[0];
//    string indexerReturnType = computeMethodReturnType(retType, meta, true);
//    _buffer << "  [index: number]: " << indexerReturnType << ";" << endl;
//  }
//
//  if (compoundInstanceMethods.find("countByEnumeratingWithStateObjectsCount") != compoundInstanceMethods.end()) {
//    _buffer << "  [Symbol.iterator](): Iterator<any>;" << endl;
//  }
  
  for (auto& methodPair : compoundInstanceMethods) {
    BaseClassMeta* owner = methodPair.second.first;
    MethodMeta* method = methodPair.second.second;

    string output = writeMethod(methodPair, meta, immediateProtocols, true);

    // TODO: more robust sytem for renaming duplicate fn names
    VueComponentFormatter::current.findAndReplaceIn(output, "drawTitleWithFrameIn(withFrame", "drawTitleWithFrameIn_(withFrame");
    VueComponentFormatter::current.findAndReplaceIn(output, "cellSize():", "cellSize_():");
    
    if (output.size()) {
       _buffer << "  // compoundInstanceMethods\n";

      if (inheritedStaticMethods.find(method->name) != inheritedStaticMethods.end()) {
        _buffer << "  //";
      }
      if (owner != meta) {
        _buffer << "  //";
      }
      
      _buffer << "  " << output << endl;
    }
  }
  
  allNamespaces[meta->module->Name] = true;
  allClasses[meta->jsName] = true;
  
  _buffer << "}" << endl << endl;
}

void DefinitionWriter::writeProperty(PropertyMeta* propertyMeta, BaseClassMeta* owner, InterfaceMeta* target, CompoundMemberMap<PropertyMeta> baseClassProperties)
{
  bool optOutTypeChecking = false;
  auto result = baseClassProperties.find(propertyMeta->name);
  if (result != baseClassProperties.end()) {
    optOutTypeChecking = result->second.second->getter->signature[0] != propertyMeta->getter->signature[0];
  }
  
  string output = writeProperty(propertyMeta, target, optOutTypeChecking);
  
  if (!output.empty()) {
    _buffer << "  ";
    
    if (clang::cast<clang::ObjCPropertyDecl>(propertyMeta->declaration)->isClassProperty()) {
      _buffer << "static ";
    }
    
    _buffer << output << endl;
  }
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

  if (meta->protocols.size()) {
    _buffer << " extends ";
    for (size_t i = 0; i < meta->protocols.size(); i++) {
      transform(
                     meta->protocols[i]->instanceProperties.begin(),
                     meta->protocols[i]->instanceProperties.end(),
                     inserter(conformedProtocolsProperties,
                                   conformedProtocolsProperties.end()
                                   ),
                     []
                     (PropertyMeta* propertyMeta) {
        return make_pair(propertyMeta->name, propertyMeta);
      });
      transform(
                     meta->protocols[i]->instanceMethods.begin(),
                     meta->protocols[i]->instanceMethods.end(),
                     inserter(conformedProtocolsMethods,
                                   conformedProtocolsMethods.end()
                                   ),
                     []
                     (MethodMeta* methodMeta) {
        return make_pair(methodMeta->name, methodMeta);
      });

      _buffer << meta->protocols[i]->jsName;
      if (i < meta->protocols.size() - 1) {
        _buffer << ", ";
      }
    }
  }
  
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
       _buffer << "  // instance method\n";
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
  argumentLabels.erase(argumentLabels.begin());
  
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
    VueComponentFormatter::current.findAndReplaceIn(returnType, ",id", ", any");
    
    // Could be done better, but need to be careful to only do this for params,
    // any maybe not for NSDictionary subclasses
    VueComponentFormatter::current.findAndReplaceIn(returnType, "NSDictionary<", "Map<");
    
    if (returnType == "NSDictionary" || returnType == "NSMutableDictionary") {
      // sometimes shows up by itself without <generic> params
      returnType = "Map<any, any>";
    }
    if (returnType == "NSArray" || returnType == "NSMutableArray") {
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
    else if (returnType == "dtdKind") {
      returnType = "XMLDTDNode.DTDKind";
    }
    else if (returnType == "NSFetchRequest") {
      returnType = "NSFetchRequest<any>";
    }

    // NSCopying is special as noted elsewhere, this fix should happen earlier
    VueComponentFormatter::current.findAndReplaceIn(returnType, "<NSCopying>", "");

//    VueComponentFormatter::current.findAndReplaceIn(returnType, "NSNetService", "NetService");
    VueComponentFormatter::current.findAndReplaceIn(returnType, "dtdKind", "XMLDTDNode.DTDKind");
    
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

  if (method->getFlags(MethodIsInitializer)) {
    // TODO: Fix string comparison, use hasClosedGenerics() instead
    if (returnType.find("<") != string::npos) {
      return output.str();
    }
    
    output << "static ";
  }
  
//  string bridgedName = method->bridgedName(hasNonArrayNonObjectGeneric);
//
//  if (bridgedName == "cellSize") {
//    bridgedName = "cellSize_";
//  }

  output << method->jsName;

  if (!methodDecl.isInstanceMethod() && owner->is(MetaType::Interface)) {
    if ((retType->is(TypeInstancetype) || DefinitionWriter::hasClosedGenerics(*retType))) {
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
  
  if ((owner->type == MetaType::Protocol && methodDecl.getImplementationControl() == clang::ObjCMethodDecl::ImplementationControl::Optional) || (owner->is(MetaType::Protocol) && method->getFlags(MethodIsInitializer))) {
    output << "?";
  }
  
  string paramsString = getInstanceParamsStr(method, owner);
  
  output << paramsString << ": ";
  
  // Method return type
  
  if (returnType == "NSFetchRequest") {
    returnType = "NSFetchRequest<any>";
  }
  
  output << returnType;
  
  output << ";";
  
  return output.str();
}

string DefinitionWriter::writeMethod(CompoundMemberMap<MethodMeta>::value_type& methodPair, BaseClassMeta* owner, const unordered_set<ProtocolMeta*>& protocols, bool canUseThisType)
{
  
  BaseClassMeta* memberOwner = methodPair.second.first;
  MethodMeta* method = methodPair.second.second;
  
  if (hiddenMethods.find(method->jsName) != hiddenMethods.end()) {
    return string();
  }
  
  bool isOwnMethod = memberOwner == owner;
  bool implementsProtocol = protocols.find(static_cast<ProtocolMeta*>(memberOwner)) != protocols.end();
  bool returnsInstanceType = method->signature[0]->is(TypeInstancetype);
  
  ostringstream output;
  
  if (isOwnMethod || implementsProtocol || returnsInstanceType) {
    bool unavailableInSwift = MetaData::getUnavailableInSwift(method, owner);
    
    if (unavailableInSwift) {
      output << "  // unavailableInSwift ";
    }
    
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
  
  bool unavailableInSwift = TypeScript::MetaData::getUnavailableInSwift(meta, owner);
  
  if (unavailableInSwift) {
    output << "  // unavailableInSwift ";
  }
  
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
    
    // Output corresponding setter for this property
    output << endl;
    output << "  " << setterName << "(_: " + returnType + ")";
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
    string renamed = MetaData::renamedName(tsified);
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
  allNamespaces[meta->module->Name] = true;
  namespaceEnums[meta->module->Name].push_back(meta);
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

bool DefinitionWriter::hasClosedGenerics(const Type& type)
{
  if (type.is(TypeInterface)) {
    const InterfaceType& interfaceType = type.as<InterfaceType>();
    if (interfaceType.interface->name == "NSCandidateListTouchBarItem") {
      return true;
    }
    return interfaceType.typeArguments.size();
  }
  
  return false;
}

string DefinitionWriter::tsifyType(const Type& type, const bool isFuncParam)
{
  switch (type.getType()) {
    case TypeVoid:
      return "void";
    case TypeBool:
      return "boolean";
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
      return "number";
    case TypeUnichar:
    case TypeSelector:
      return "string";
    case TypeCString: {
      string res = "string";
      if (isFuncParam) {
        Type typeVoid(TypeVoid);
        res += " | " + JSExportFormatter::current.nameForJSExport(MetaData::lookupApiNotes(tsifyType(::Meta::PointerType(&typeVoid), isFuncParam)));
      }
      return res;
    }
    case TypeProtocol:
      return "any /* Protocol */";
    case TypeClass:
      return "typeof NSObject";
    case TypeId: {
      const IdType& idType = type.as<IdType>();
      if (idType.protocols.size() == 1) {
        string protocol = idType.protocols[0]->jsName;
        if (protocol != "NSCopying") {
          return protocol;
        }
      }
      return "any";
    }
    case TypeConstantArray:
    case TypeExtVector:
      //        return "interop.Reference<" + tsifyType(*type.as<ConstantArrayType>().innerType) + ">";
      return JSExportFormatter::current.nameForJSExport(MetaData::lookupApiNotes(tsifyType(*type.as<ConstantArrayType>().innerType)));
    case TypeIncompleteArray:
      //        return "interop.Reference<" + tsifyType(*type.as<IncompleteArrayType>().innerType) + ">";
      return JSExportFormatter::current.nameForJSExport(MetaData::lookupApiNotes(tsifyType(*type.as<IncompleteArrayType>().innerType)));
    case TypePointer: {
      const PointerType& pointerType = type.as<PointerType>();
      return (pointerType.innerType->is(TypeVoid)) ? "any" : JSExportFormatter::current.nameForJSExport(MetaData::lookupApiNotes(tsifyType(*pointerType.innerType)));
    }
    case TypeBlock:
      return JSExportFormatter::current.nameForJSExport(MetaData::lookupApiNotes(writeFunctionProto(type.as<BlockType>().signature)));
    case TypeFunctionPointer:
      return JSExportFormatter::current.nameForJSExport(MetaData::lookupApiNotes(writeFunctionProto(type.as<FunctionPointerType>().signature)));
    case TypeInterface:
    case TypeBridgedInterface: {
      if (type.is(TypeType::TypeBridgedInterface) && type.as<BridgedInterfaceType>().isId()) {
        return JSExportFormatter::current.nameForJSExport(MetaData::lookupApiNotes(tsifyType(IdType())));
      }

      const InterfaceMeta& interface = type.is(TypeType::TypeInterface) ? *type.as<InterfaceType>().interface : *type.as<BridgedInterfaceType>().bridgedInterface;
      
      if (interface.name == "NSNumber") {
        return "number";
      }
      else if (interface.name == "NSString") {
        return "string";
      }
      else if (interface.name == "NSDate") {
        return "Date";
      }

      ostringstream output;
      bool hasClosed = hasClosedGenerics(type);
      string firstElementType;
      string interfaceName = interface.jsName;
      
      if (interfaceName == "NSDictionary") {
        interfaceName = "Map";
      }

      if (interface.name == "NSArray") {
        if (hasClosed) {
          const InterfaceType& interfaceType = type.as<InterfaceType>();
          string arrayType = tsifyType(*interfaceType.typeArguments[0]);
          arrayType = MetaData::renamedName(interface.name + "." + arrayType);

          output << arrayType << "[]";
        } else {
          output << "any[]";
        }
      }
      else if (hasClosed) {
        output << interfaceName;
        
        const InterfaceType& interfaceType = type.as<InterfaceType>();

        output << "<";

        for (size_t i = 0; i < interfaceType.typeArguments.size(); i++) {
          string argType = JSExportFormatter::current.nameForJSExport(MetaData::lookupApiNotes(tsifyType(*interfaceType.typeArguments[i])));
          output << argType;
          if (i == 0) {
            firstElementType = argType;//we only need this for NSArray
          }
          if (i < interfaceType.typeArguments.size() - 1) {
            output << ", ";
          }
        }

        output << ">";
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

      return output.str();
    }
    case TypeStruct: {
      const StructType& structType = type.as<StructType>();
      return structType.structMeta->jsName;
//      if (structType.structMeta->swiftModule == "_global" && structType.structMeta->module->Name == "_global") {
//        return structType.structMeta->jsName;
//      }
//      return structType.structMeta->module->Name + "." + structType.structMeta->jsName;
    }
    case TypeUnion:
      return (*type.as<UnionType>().unionMeta).jsName;
    case TypeAnonymousStruct:
    case TypeAnonymousUnion: {
      ostringstream output;
      output << "{ ";

      const vector<RecordField>& fields = type.as<AnonymousStructType>().fields;
      for (auto& field : fields) {
        output << JSExportFormatter::current.nameForJSExport(MetaData::lookupApiNotes(field.name)) << ": " << JSExportFormatter::current.nameForJSExport(MetaData::lookupApiNotes(tsifyType(*field.encoding))) << "; ";
      }

      output << "}";
      return output.str();
    }
    case TypeEnum: {
      const EnumType& enumType = type.as<EnumType>();
      return enumType.enumMeta->jsName;
//      if (enumType.enumMeta->swiftModule == "_global" && enumType.enumMeta->module->Name == "_global") {
//        return enumType.enumMeta->jsName;
//      }
//      return enumType.enumMeta->module->Name + "." + enumType.enumMeta->jsName;
    }
    case TypeTypeArgument:
      return JSExportFormatter::current.nameForJSExport(MetaData::lookupApiNotes(type.as<TypeArgumentType>().name));
    case TypeVaList:
    case TypeInstancetype:
    default:
      break;
  }

  assert(false);
  return "";
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
    output << tsifyType(*retType);
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
  
  for (::Meta::Meta* meta : _module.second) {
    if (!meta->is(Enum)) {
      if (JSExportDefinitionWriter::hiddenClasses.find(meta->jsName) == JSExportDefinitionWriter::hiddenClasses.end()) {
        meta->visit(this);
      }
    }
  }
  
  return _buffer.str();
}

string DefinitionWriter::writeExports() {
  ostringstream output;

  output << "export {\n";
  
  size_t i = 1;
  
  for (const auto& namespaceRecord : allNamespaces) {
    i++;
    
    string renamedNamespaceName = MetaData::renamedName(namespaceRecord.first);
    
    bool hasDecls = namespaceEnums[renamedNamespaceName].size()
      || structsMeta[renamedNamespaceName].size()
      || namespaceVars[renamedNamespaceName].size()
      || namespaceTypealiases[renamedNamespaceName].size()
      || allClasses[renamedNamespaceName]
      || allInterfaces[renamedNamespaceName];
    
    if (!hasDecls || namespaceRecord.first == "NSNetService") {
      continue;
    }
  
    output << "  " << namespaceRecord.first;
    
    if (i < allNamespaces.size() - 2) {
      output << ",";
    }
    
    output << "\n";
  }
  
  output << "};\n";
  
  namespaceEnums.empty();
  namespaceVars.empty();
  namespaceTypealiases.empty();
  allNamespaces.empty();
  allClasses.empty();
  allInterfaces.empty();
  
  namespaceEnums = {};
  namespaceVars = {};
  namespaceTypealiases = {};
  allNamespaces = {};
  allClasses = {};
  allInterfaces = {};
  
  return output.str();
}

string DefinitionWriter::writeNamespaces(bool writeToClasses)
{
  populateStructsMeta();
  populateTypealiases();
  
  ostringstream output;

  for (const auto& namespaceRecord : allNamespaces) {
    map<string, bool> writtenEnums = {};

    string renamedNamespaceName = MetaData::renamedName(namespaceRecord.first);
    bool hasDecls = namespaceEnums[renamedNamespaceName].size() ||
      structsMeta[renamedNamespaceName].size() ||
      namespaceVars[renamedNamespaceName].size() ||
      namespaceTypealiases[renamedNamespaceName].size();
    
    if (!hasDecls || namespaceRecord.first == "NSNetService") {
      continue;
    }
    
    output << "namespace " << renamedNamespaceName << " {\n";
    
    // Namespace enums
    
    for (const auto& namespaceEnum : namespaceEnums[renamedNamespaceName]) {
      // Enum values
      vector<EnumField>& fields = namespaceEnum->swiftNameFields.size() != 0 ? namespaceEnum->swiftNameFields : namespaceEnum->fullNameFields;
      string enumName = namespaceEnum->jsName;
      
      output << "  export enum " << enumName << " {\n";
      
      for (size_t i = 0; i < fields.size(); i++) {
        output << "    " +  fields[i].name + " = " + fields[i].value;
        
        if (i < fields.size() - 1) {
          output << ",";
        }
        
        output << "\n";
      }
      
      writtenEnums[enumName] = true;
      
      output << "  }\n\n";
      
      if (writeToClasses) {
        output << "  " << "global['" << renamedNamespaceName << "']['" << enumName << "'] = " << enumName << ";\n\n";
      }
    }
    
    // Namespace vars

    map<string, bool> writtenVars = {};

    for (const auto& namespaceVar : namespaceVars[renamedNamespaceName]) {
      string varName = sanitizeParameterName(namespaceVar->jsName);

      if (writtenVars[varName]) {
        continue;
      }
      
      output << "  export let " << varName << ": " << jsifyTypeName(tsifyType(*namespaceVar->signature)) << ";\n";

      writtenVars[varName] = true;
    }
    
    // TODO: Namespace objc structs
//    for (const auto& structMeta : objcStructs[namespaceName]) {
//    }
    
    for (const auto& namespaceStruct : structsMeta[renamedNamespaceName]) {
      string enumName = namespaceStruct.first;
      
      if (writtenEnums[enumName]) {
        // Already written above
        continue;
      }
      
      output << "  // struct " << endl;
      
      auto values = namespaceStruct.second["values"].as<vector<string>>();
      auto type = namespaceStruct.second["type"].as<string>();
      auto valuesStr = StringUtils::join(values, ",\n    ");
    
      output << "  export enum " << enumName << " {\n";
      output << "    " << valuesStr << endl;
      output << "  }\n\n";
    }
    
    // Namespace type aliases
    
    for (const auto& typeAliases : namespaceTypealiases[renamedNamespaceName]) {
      output << "  export type " << typeAliases.first << " = " << jsifyTypeName(typeAliases.second) <<  ";\n";
    }
    
    output << "}\n\n";
  }

  // Globals

  for (const auto& structClass : structsMeta["_global"]) {
    string structClassName = structClass.first;
    
    map<string, bool> previousNames = {};
    
    auto values = structClass.second["values"].as<vector<string>>();
    
    if (values.empty()) {
      continue;
    }
    
    output << "export enum " << structClassName << " {\n";
    
    for (const auto& value : values) {
      if (previousNames[value]) {
        continue;
      }
      
      previousNames[value] = true;
      
      output << "  " << value << ",\n";
    }
    
    output << "}\n\n";
    
    if (writeToClasses) {
      output << "global['" << structClassName << "'] = " << structClassName << ";\n\n";
    }
  }
  
  for (const auto& namespaceEnum : namespaceEnums["_global"]) {
    // Enum values
    vector<EnumField>& fields = namespaceEnum->swiftNameFields.size() != 0 ? namespaceEnum->swiftNameFields : namespaceEnum->fullNameFields;
    string enumName = namespaceEnum->jsName;
    
    output << "export enum " << enumName << " {\n";
    
    for (size_t i = 0; i < fields.size(); i++) {
      output << "  " +  fields[i].name + " = " + fields[i].value;
      
      if (i < fields.size() - 1) {
        output << ",";
      }
      
      output << "\n";
    }
    
    output << "}\n\n";
    
    if (writeToClasses) {
      output << "global['" << enumName << "'] = " << enumName << ";\n\n";
    }
  }
  
  for (const auto& namespaceVar : namespaceVars["_global"]) {
    string varName = sanitizeParameterName(namespaceVar->jsName);
    
    output << "// export let " << varName << ": " << jsifyTypeName(tsifyType(*namespaceVar->signature)) << ";\n";
    
    if (writeToClasses) {
      output << "// global['" << varName << "'] = " << varName << ";\n\n";
    }
  }
  

  return output.str();
}

}
