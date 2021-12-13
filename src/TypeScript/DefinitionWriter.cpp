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

bool DefinitionWriter::applyManualChanges = false;

string dataRoot = getenv("DATAPATH");

static map<string, map<string, map<string, EnumMeta*>>> namespaceEnums = {};
static map<string, vector<VarMeta*>> namespaceVars = {};
static map<string, vector<string>> namespaceClasses = {};
static vector<InterfaceMeta*> namespaceViews = {};
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
  "createWithNibName_Bundle",
  "prototype",
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

string DefinitionWriter::jsifySwiftTypeName(const string& jsName)
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
  
  bool isProtoClass = JSExportDefinitionWriter::overlaidClasses.find(jsName) != JSExportDefinitionWriter::overlaidClasses.end();
  
  // swift overlay classes
  if (isProtoClass) {
    return "NS" + jsName;
  }

  return jsName;
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

string DefinitionWriter::getTypeString(clang::ASTContext &Ctx, clang::Decl::ObjCDeclQualifier Quals, clang::QualType qualType, const Type& type, const bool isFuncParam) {
  clang::QualType pointerType = Ctx.getUnqualifiedObjCPointerType(qualType);
  
  if (pointerType.getAsString().find("instancetype ", 0) != string::npos) {
    return "Self";
  }
  
  string formattedOut = Type::tsifyType(type);
  
  return formattedOut;
}

bool DefinitionWriter::getTypeOptional(clang::ASTContext &Ctx, clang::Decl::ObjCDeclQualifier Quals, clang::QualType qualType, const Type& type, const bool isFuncParam) {
  bool isOptional = false;
  
  string formattedOut = Type::tsifyType(type);
  
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
    
    string returnType = Type::tsifyType(*pType, false, true);
    returnType = jsifySwiftTypeName(returnType);
    
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

// MARK: -

// MARK: Visit Interface

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
  
  if (metaName == "Set" || metaName == "String" || metaName == "NSObject" || metaName == "NSSimpleCString" ||
      metaName == "Date" || metaName == "Error" || metaName == "Array" || metaName == "NSMutableString") {
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
  
  bool isProtoClass = JSExportDefinitionWriter::overlaidClasses.find(meta->jsName) != JSExportDefinitionWriter::overlaidClasses.end();

  if (isProtoClass) {
    metaName = meta->name;
  }
  
  string parametersString = getTypeParametersStringOrEmpty(clang::cast<clang::ObjCInterfaceDecl>(meta->declaration));

  if (metaName == "NSBitmapImageRep") {
    out << "// @ts-ignore\n";
  }
  
  out << "export class " << metaName;
  
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
      
      if (methodPair.second.second->jsName == "imageReps") {
        out << "// @ts-ignore \n  ";
      }
      
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
    
    bool skipProperty = false;

    for (auto& methodPair : compoundInstanceMethods) {
      MethodMeta* method = methodPair.second.second;

      if (method->jsName == propertyMeta->jsName) {
        cerr << "Skipping " << method->jsName << endl;
        skipProperty = true;
        break;
      }
    }
    
    if (skipProperty) {
      continue;
    }

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
    
    bool skipProperty = false;
    
    for (auto& methodPair : compoundInstanceMethods) {
      MethodMeta* method = methodPair.second.second;
      
      if (method->jsName == propertyMeta->jsName) {
        cerr << "Skipping " << method->jsName << endl;
        skipProperty = true;
        break;
      }
    }
    
    if (skipProperty) {
      continue;
    }

//    out << "  // protocolInheritedInstanceProperties\n";
    
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
  
  for (auto& methodPair : compoundInstanceMethods) {
    BaseClassMeta* owner = methodPair.second.first;
    MethodMeta* method = methodPair.second.second;
    
    bool skipMethod = false;

    for (PropertyMeta* property : meta->instanceProperties) {
      if (property->jsName == method->jsName) {
        cerr << "Skipping " << method->jsName << endl;
        skipMethod = true;
        break;
      }
    }

    if (skipMethod) {
      continue;
    }

    string output = writeMethod(methodPair, meta, immediateProtocols, true);
    
    if (output.size()) {
//      out << "  // compoundInstanceMethods\n";
      
      if (meta->nameExistsInSuperclass(method->jsName, Property)) {
        out << "  // @ts-ignore \n";
      }
      else if (meta->nameExistsInSuperclass(method->jsName, Method)) {
        out << "  // @ts-ignore \n";
      }
      
      // TODO: fix temp ugly hack
      if (method->builtName() == "tag") {
        out << "  // ";
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
  
  out << "export function " << metaName << parametersString << "(args?: any): " << metaName << parametersString << ";" << endl << endl;
  
  if (containerName != metaName && !isProtoClass) {
    namespaceClasses[containerName].push_back(out.str());
    return;
  }
  
  _buffer << out.str();
}

// MARK: Visit Protocol

void DefinitionWriter::visit(ProtocolMeta* meta)
{
  string metaName = meta->jsName;
  
  allInterfaces[metaName] = true;
  
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

// MARK: Visit Enum

void DefinitionWriter::visit(EnumMeta* meta)
{
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
    vector<string> parentNameParts;
    StringUtils::split(meta->jsName, '.', back_inserter(parentNameParts));

    if (parentNameParts.size() > 1) {
      string parentName = parentNameParts[0];

      for (const auto& parentEnum: namespaceEnums[parentName]["_container"]) {
        if (parentEnum.second != NULL) {
          namespaceEnums[moduleName][enumContainer][parentEnum.first] = parentEnum.second;
        }
      }
    }

    if (enumContainer != "_container") {
      namespaceEnums[enumContainer]["_container"][enumName] = meta;
    }
  }
  
  namespaceEnums[moduleName][enumContainer][enumName] = meta;
}

// MARK: Visit Var

void DefinitionWriter::visit(VarMeta* meta)
{
  allNamespaces[meta->module->Name] = true;
  namespaceVars[meta->module->Name].push_back(meta);
}

// MARK: Visit Method

void DefinitionWriter::visit(MethodMeta* meta) { }

// MARK: Visit Property

void DefinitionWriter::visit(PropertyMeta* meta) {   }

// MARK: Visit EnumConstant

void DefinitionWriter::visit(EnumConstantMeta* meta) { }

// MARK: Visit Struct

void DefinitionWriter::visit(StructMeta* meta)
{
  allNamespaces[meta->module->Name] = true;
}

// MARK: Visit Union

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

// MARK: Visit Category

void DefinitionWriter::visit(CategoryMeta* meta) { }

// MARK: Visit Function

void DefinitionWriter::visit(FunctionMeta* meta)
{
  allNamespaces[meta->module->Name] = true;
  
  const clang::FunctionDecl& functionDecl = *clang::dyn_cast<clang::FunctionDecl>(meta->declaration);
  ostringstream params;
  
  for (size_t i = 1; i < meta->signature.size(); i++) {
    string name = sanitizeParameterName(functionDecl.getParamDecl(i - 1)->getNameAsString());
    string tsified = Type::tsifyType(*meta->signature[i], true);
    string renamed = renamedName(tsified);
    string paramTypeStr = jsifySwiftTypeName(renamed);
    
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
    returnName = Type::tsifyType(*meta->signature[0]);
  }
  
  _buffer << returnName << ";\n\n";
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
    string typeStr = Type::tsifyType(*retType);
    
    // Add prefix for types that are in same namespace as this class name
    //
    // e.g. NSTypesetterControlCharacterAction -> NSTypesetter.NSTypesetterControlCharacterAction
    //
    if (namespaceEnums[owner->jsName]["_container"][typeStr]) {
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

// MARK: - Write

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
  
  if (isOwnMethod || implementsProtocol || returnsInstanceType) {
    output << writeMethod(method, owner, canUseThisType);
  }
  
  return output.str();
}

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
  
  if (owner->jsName == "NSWorkspace" && method->jsName == "duplicate") {
    cout << "";
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
  
  // For some reason, has different params than the method it is overriding
  if (owner->jsName == "NSMenuItemCell") {
    if (method->builtName() == "drawImageWithFrameInView" ||
        method->builtName() == "drawTitleWithFrameInView") {
      output << "// ";
    }
  }
  
  if (owner->jsName == "NSSegmentedControl") {
    if (method->jsName == "setAlignment" ||
        method->jsName == "setEnabled" ||
        method->jsName == "setTag") {
      output << "// @ts-ignore \n  ";
    }
  }
  
  if (owner->jsName == "NSSegmentedCell") {
    if (method->jsName == "setEnabled" ||
        method->jsName == "setTag") {
      output << "// @ts-ignore \n  ";
    }
  }
  
  if (owner->jsName == "NSTextView") {
    if (method->jsName == "setAlignment" ||
        method->jsName == "setBaseWritingDirection") {
      output << "// @ts-ignore \n  ";
    }
  }
  
  if (owner->jsName == "NSMutableSet") {
    if (method->jsName == "add") {
      output << "// @ts-ignore \n  ";
    }
  }
  
  if (owner->jsName == "NSBitmapImageRep") {
    if (method->jsName == "add") {
      output << "// @ts-ignore \n  ";
    }
  }
  
  if (method->jsName == "isEnabled") {
    output << "// @ts-ignore \n  ";
  }
  
  const Type* retType = method->signature[0];
  string returnType = computeMethodReturnType(retType, owner, canUseThisType);
  returnType = jsifySwiftTypeName(returnType);
  
  if (method->getFlags(MethodIsInitializer)) {
    // TODO: Fix string comparison, use hasClosedGenerics() instead
    if (returnType.find("<") != string::npos) {
      output << "// ";
    }
    
    output << "static ";
  }
  
  if (method->isInit() ||
      owner->jsName == "NSLayoutConstraint" ||
      owner->jsName == "NSView") {
    output << method->builtName();
  }
  else {
    output << method->jsName;
  }
  
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
  
  bool isProtoClass = JSExportDefinitionWriter::overlaidClasses.find(returnType) != JSExportDefinitionWriter::overlaidClasses.end();
  
  // swift overlay classes
  if (isProtoClass) {
    returnType = "NS" + returnType;
  }

  output << returnType;
  
  output << ";";
  
  return output.str();
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

string DefinitionWriter::writeProperty(PropertyMeta* meta, BaseClassMeta* owner, bool optOutTypeChecking)
{
  ostringstream output;
  
  if (hiddenMethods.find(meta->jsName) != hiddenMethods.end()) {
    return string();
  }
  
  if (meta->getUnavailableInSwift(owner)) {
    output << "  // unavailableInSwift ";
  }
  
  if (meta->jsName == "isEnabled") {
    output << "// @ts-ignore \n  ";
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
  
  returnType = jsifySwiftTypeName(returnType);
  
  output << returnType;
  output << ";";
  
  if (meta->setter && !meta->setter->jsName.empty()) {
    string setterName = meta->setter->name;
    if (setterName == "setQueryItems") {
      cout << "";
    }
    setterName.pop_back();
    
    // Write setter too
    output << endl;
    output << "  " << setterName << "(_: " + returnType + ");";
  }
  
  return output.str();
}

string DefinitionWriter::writeMembers(const vector<RecordField>& fields, vector<TSComment> fieldsComments)
{
  string members;
  
  for (size_t i = 0; i < fields.size(); i++) {
    if (fields[i].name[0] != '_') {
      members += "  " + fields[i].name + ": " + Type::tsifyType(*fields[i].encoding) + ";\n";
    }
  }
  
  return members;
}

string DefinitionWriter::writeExports() {
  ostringstream output;
  
  for (auto& namespaceView : namespaceViews) {
    output << "export var " << namespaceView->shortName() << " = " << namespaceView->jsName << "\n\n";
  }

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

    bool hasDefinedEnums = false;
    
    for (const auto& namespaceEnum : namespaceEnums[namespaceName]) {
      if (namespaceEnum.second.empty()) {
        hasDefinedEnums = false;
        break;
      }
    }
    
    bool hasDecls = namespaceEnums[namespaceName].size() ||
      namespaceClasses[namespaceName].size();

//    bool hasDecls = namespaceEnums[namespaceName].size()
//      || namespaceEnums[namespaceName].size()
//      || namespaceVars[namespaceName].size()
//      || namespaceTypealiases[namespaceName].size()
//      || allClasses[namespaceName]
//      || allInterfaces[namespaceName];
    
    if (!hasDecls ||
        namespaceName == "XMLNode.Options" ||
        namespaceRecord.first == "NSNetService" ||
        namespaceRecord.first == "runtime") {
      continue;
    }
    
    if (namespaceName.substr(0, 7) == "_global") {
      continue;
    }
    
    // TODO: make an "ignoredNamespaces" or something
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
  namespaceVars.empty();
  namespaceTypealiases.empty();
  allClasses.empty();
  allInterfaces.empty();
  
  namespaceClasses = {};
  namespaceVars = {};
  namespaceTypealiases = {};
  allClasses = {};
  allInterfaces = {};
  
  return output.str();
}

// MARK: - Write Namespaces
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
      namespaceClasses[namespaceName].size();
    
//      namespaceVars[namespaceName].size() ||
//      namespaceTypealiases[namespaceName].size();
    
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

    // MARK: Namespace Classes

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
    
    // MARK: Namespace Enums
    
    if (writeToClasses && namespaceEnums[namespaceName].size()) {
      output << "(globalThis as any)['" << namespaceName << "'] = (globalThis as any)['" << namespaceName << "'] || {};" << endl;
    }

    for (const auto& namespaceEnum : namespaceEnums[namespaceName]) {
      string className = namespaceEnum.first;
      bool isContainer = className == "_container";
      const string SP = isContainer ? "" : "  ";

      if (namespaceEnum.second.size()) {
        if (!writeToClasses && !isContainer) {
          output << SP << "export namespace " << className << " {" << endl;
        }
      }
      
      for (const auto& namespaceContainer : namespaceEnum.second) {
        string enumName = namespaceContainer.first;
        auto enumVal = namespaceContainer.second;
        
        if (!enumVal) {
          continue;
        }
        
        if (writtenEnums[namespaceName + "." + className + "." + enumName]) {
          continue;
        }

        writtenEnums[namespaceName + "." + className + "." + enumName] = true;

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

        vector<EnumField>& fields = enumVal->swiftNameFields.size() != 0 ? enumVal->swiftNameFields : enumVal->fullNameFields;

        if (writeToClasses) {
          output << "(globalThis as any)['" << namespaceName << "']";

          if (!isContainer) {
            output << "['" << className << "']";
          }

          output << "['" << enumName << "']";
          output << " = {\n";

          for (size_t i = 0; i < fields.size(); i++) {
            output << "  " +  fields[i].name + ": " + fields[i].value;
            output << ",";
            output << "\n";
          }

          map<string, bool> writtenValues = {};

          for (size_t i = 0; i < fields.size(); i++) {
            if (writtenValues[fields[i].value]) {
              continue;
            }
            writtenValues[fields[i].value] = true;
            output << "  '" +  fields[i].value + "': '" + fields[i].name + "'";
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

      if (namespaceEnum.second.size()) {
        if (!writeToClasses && !isContainer) {
          output << SP << "}" << endl << endl;
        }
      }
    }

    if (!writeToClasses) {
      output << "}" << endl;
    }

    // MARK: Namespace Vars
    
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
//      output << "//  export let " << varName << ": " << jsifyTypeName(Type::tsifyType(*namespaceVar->signature)) << ";\n";
//
//      writtenVars[varName] = true;
//    }
    
    // MARK: Namespace ObjC Structs
    
//    for (const auto& structMeta : objcStructs[namespaceName]) {
//    }
    
    
    // MARK: Namespace Typealiases
    
//
//    for (const auto& typeAliases : namespaceTypealiases[namespaceName]) {
//      output << "  export type " << typeAliases.first << " = " << jsifyTypeName(typeAliases.second) <<  ";\n";
//    }
    
    output << "\n";
  }

  // MARK: Global Structs
  
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
//  MARK: Global Enums
  
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
//  MARK: Global Vars
  
//  for (const auto& namespaceVar : namespaceVars["_global"]) {
//    string varName = sanitizeParameterName(namespaceVar->jsName);
//
//    output << "// export let " << varName << ": " << jsifyTypeName(Type::tsifyType(*namespaceVar->signature)) << ";\n";
//
//    if (writeToClasses) {
//      output << "// global['" << varName << "'] = " << varName << ";\n\n";
//    }
//  }

  return output.str();
}

string DefinitionWriter::visitAll()
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

}
