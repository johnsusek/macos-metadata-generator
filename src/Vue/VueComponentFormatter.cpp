#include "TypeScript/DefinitionWriter.h"
#include "VueComponentDefinitionWriter.h"
#include "Meta/Utils.h"
#include "Meta/NameRetrieverVisitor.h"
#include "Utils/StringUtils.h"
#include <algorithm>
#include <clang/AST/DeclObjC.h>
#include <iterator>
#include <regex>
#include "yaml-cpp/yaml.h"
#include "VueComponentFormatter.h"

namespace TypeScript {
using namespace Meta;
using namespace std;

VueComponentFormatter VueComponentFormatter::current = VueComponentFormatter();

map<string, bool> VueComponentFormatter::nativeTypes = {
  { "object", true },
  { "array", true },
  { "number", true },
  { "String", true },
  { "string", true },
  { "boolean", true }
};

string VueComponentFormatter::vuePropifyTypeName(const string& jsName)
{
  static map<string, string> jsNames = {
    { "CGDict", "Object" },
    { "NSDictionary", "Object" },
    { "NSArray", "Array" },
    { "CGFloat", "Number" },
    { "Float", "Number" },
    { "float", "Number" },
    { "Double", "Number" },
    { "double", "Number" },
    { "Int", "Number" },
    { "NSInteger", "Number" },
    { "NSUInteger", "Number" },
    { "Int32", "Number" },
    { "Int64", "Number" },
    { "UInt", "Number" },
    { "UInt32", "Number" },
    { "UInt64", "Number" },
    { "NSString", "String" },
    { "Selector", "String" },
    { "Any", "Object" },
    { "AnyClass", "Object" },
    { "AnyObject", "Object" },
    { "Bool", "Boolean" }
  };
  
  if (!jsNames[jsName].empty()) {
    return jsNames[jsName];
  }
  
  return jsName;
}

vector<string> VueComponentFormatter::split2(string const &input) {
  istringstream buffer(input);
  
  vector<string> ret{
    istream_iterator<string>(buffer),
    istream_iterator<string>()
  };
  
  return ret;
}

string VueComponentFormatter::formatTypeId(const IdType& idType, const clang::QualType pointerType, const bool ignorePointerType) {
  string out = "";
  
  const clang::Type* typePtr = pointerType.getTypePtr();
  const clang::QualType pointeeType = typePtr->getPointeeType();
  
  if (!ignorePointerType && pointeeType.getAsString()[0] != 'i' && pointeeType.getAsString()[1] != 'd') {
    vector<string> tokens;
    StringUtils::split(pointerType.getAsString(), ' ', back_inserter(tokens));
    
    return tokens[0];
  }
  else if (idType.protocols.empty()) {
    return "any";
  }
  else if (idType.protocols.size() == 1) {
    const ::Meta::Meta& meta = *idType.protocols[0];
    
    // We pass string to be marshalled to NSString which conforms to NSCopying. NSCopying is tricky.
    if (meta.jsName != "NSCopying") {
      return meta.jsName;
    }
    else {
      return "any";
    }
  }
  else {
    return "any";
  }
}

string VueComponentFormatter::formatTypePointer(const PointerType& pointerType, const clang::QualType pointerQualType, const bool ignorePointerType) {
  return Type::lookupApiNotes(formatType(*pointerType.innerType, pointerQualType, true));
}

string VueComponentFormatter::formatType(const Type& type, const clang::QualType pointerType, const bool ignorePointerType)
{
  string typeStr = "";
  
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
    case TypeULongLong:
    case TypeLong:
    case TypeULong:
    case TypeLongLong:
    case TypeFloat:
    case TypeDouble:
      typeStr = "number";
      break;
    case TypeSelector:
    case TypeUnichar:
    case TypeCString:
      typeStr = "string";
      break;
    case TypeProtocol:
      typeStr = "any";
      break;
    case TypeClass: {
      auto it = type.as<PointerType>().innerType;
      auto pointeeType = pointerType->getPointeeType();
      
      if (it != NULL && pointeeType.getAsString() != "Class") {
        string str = pointeeType.getAsString();
        str.resize(str.size() - 1);
        str.replace(0, 6, "");
        return "typeof " + Type::lookupApiNotes(str);
      }
      else if (pointeeType.getAsString() == "Class") {
        return "any";
      }
      else {
        typeStr = "NSObject";
      }
      break;
    }
    case TypeStruct: {
      const StructType& structType = type.as<StructType>();
      return structType.structMeta->jsName;
    }
    case TypeUnion:
      typeStr = type.as<UnionType>().unionMeta->jsName;
      break;
    case TypeEnum: {
      const EnumType& enumType = type.as<EnumType>();
      return enumType.enumMeta->jsName;
    }
    case TypeTypeArgument:
      typeStr = type.as<TypeArgumentType>().name;
      break;
    case TypeBlock:
      typeStr = getFunctionProto(type.as<BlockType>().signature, pointerType);
      break;
    case TypeFunctionPointer:
      typeStr = getFunctionProto(type.as<FunctionPointerType>().signature, pointerType);
      break;
    case TypeConstantArray:
    case TypeExtVector:
      typeStr = Type::lookupApiNotes(formatType(*type.as<ConstantArrayType>().innerType, pointerType));
      break;
    case TypeIncompleteArray:
      typeStr = formatType(*type.as<IncompleteArrayType>().innerType, pointerType);
      break;
    case TypeId:
      typeStr = VueComponentFormatter::current.formatTypeId(type.as<IdType>(), pointerType, ignorePointerType);
      break;
    case TypePointer:
      typeStr = formatTypePointer(type.as<PointerType>(), pointerType, ignorePointerType);
      break;
    case TypeInterface:
    case TypeBridgedInterface:
      typeStr = formatTypeInterface(type, pointerType);
      break;
    case TypeAnonymousStruct:
    case TypeAnonymousUnion:
      typeStr = formatTypeAnonymous(type, pointerType);
      break;
    case TypeVaList:
    case TypeInstancetype:
    default:
      break;
  }
  
  return Type::lookupApiNotes(typeStr);
}

string VueComponentFormatter::formatTypeAnonymous(const Type& type, const clang::QualType pointerType) {
  string output;
  
  output += "{ ";
  
  const vector<RecordField>& fields = type.as<AnonymousStructType>().fields;
  
  for (auto& field : fields) {
    output += field.name + ": " + formatType(*field.encoding, pointerType) + "; ";
  }
  
  output += "}";
  
  return output;
}

void VueComponentFormatter::findAndReplaceIn2(string& str, string searchFor, string replaceBy)
{
  size_t found = str.find(searchFor);
  while (found != string::npos) {
    str.replace(found, searchFor.length(), replaceBy);
    found = str.find(searchFor, found + replaceBy.length());
  };
}

string VueComponentFormatter::formatTypeInterface(const Type& type, const clang::QualType pointerQualType) {
  if (type.is(TypeType::TypeBridgedInterface) && type.as<BridgedInterfaceType>().isId()) {
    return formatType(IdType(), pointerQualType);
  }
  
  string pointerQualTypeStr = pointerQualType.getAsString();
  const InterfaceMeta& interface = type.is(TypeType::TypeInterface) ? *type.as<InterfaceType>().interface : *type.as<BridgedInterfaceType>().bridgedInterface;
  string interfaceName = vuePropifyTypeName(renamedName(interface.jsName));
  
  if (type.hasClosedGenerics()) {
    const InterfaceType& interfaceType = type.as<InterfaceType>();
    
    if (interfaceName == "NSArray" || interfaceName == "Array") {
      string out = "";
      out = "[";
      out += formatType(*interfaceType.typeArguments[0], pointerQualType);
      out += "]";
      return out;
    }
    
    if (interfaceName == "NSDictionary" || interfaceName == "Dictionary") {
      string out = "";
      out = "[";
      out += formatType(*interfaceType.typeArguments[0], pointerQualType);
      out += ": ";
      out += formatType(*interfaceType.typeArguments[1], pointerQualType, true);
      out += "]";
      return out;
    }
    
    string typeArgName;
    
    if (interfaceType.typeArguments.empty()) {
      // for NSCandidateListTouchBarItem which is generic,
      // but doesn't have type args?
      typeArgName = "AnyObject";
    }
    else {
      typeArgName = formatType(*interfaceType.typeArguments[0], pointerQualType);
      Type::stripModifiersFromPointerType(typeArgName);
    }
    
    string out = interfaceName + "<" + typeArgName + ">";
    
    return out;
  }
  
  // No generics, so we just want the inner arg type
  // e.g. NSArray<NSAppearanceName> -> NSAppearanceName
  
//  if (ignorePointerType) {
//    return interfaceName;
//  }
  
  auto typePtr = pointerQualType.getTypePtrOrNull();
  
  if (typePtr->isObjCObjectPointerType()) {
    // At this point, pointerQualTypeStr looks like this:
    // NSArray<NSString *> * _Nonnull
    //
    // and pointeeTypeStr looks like this:
    // NSArray<NSString *>
    //
    // We will get the inner `NSString` here
    //
    auto typePtrInterfaceType = typePtr->getAsObjCInterfacePointerType();
    auto interfaceObjectType = typePtrInterfaceType->getObjectType();
    auto ojectArgs = interfaceObjectType->getTypeArgsAsWritten();
    
    
    if (ojectArgs.size()) {
      string firstArg = ojectArgs[0].getAsString(); // `NSString *`
      Type::stripModifiersFromPointerType(firstArg); // `NSString`
      auto argName = vuePropifyTypeName(renamedName(firstArg)); // `String`
      
      return argName;
    }
    else {
      string pointeeTypeName = typePtr->getPointeeType().getAsString();
      if (interfaceName == "NSObject") {
        Type::stripModifiersFromPointerType(pointeeTypeName);
        return vuePropifyTypeName(renamedName(pointeeTypeName));
      }
      else {
        Type::stripModifiersFromPointerType(pointerQualTypeStr);
        return vuePropifyTypeName(renamedName(pointerQualTypeStr));
      }
    }
  }
  else {
    Type::stripModifiersFromPointerType(pointerQualTypeStr);
    return vuePropifyTypeName(renamedName(pointerQualTypeStr));
  }
  
  throw logic_error(string("Misparsed interface definition '") + interfaceName + "'.");

  //  const InterfaceMeta& interface = type.is(TypeType::TypeInterface) ? *type.as<InterfaceType>().interface : *type.as<BridgedInterfaceType>().bridgedInterface;
  //  out = interface.jsNameRenamed;
  //
  //  bool isArray = (out == "NSArray");
  //  bool isDictionary = (out == "NSDictionary");
  //  bool isNSObject = (out == "NSObject");

//  if (DefinitionWriter::hasClosedGenerics(type)) {
//    string returnKey = vuePropifyTypeName(innerReturnType[0]);
//
//    if (nativeTypes[returnKey]) {
//      transform(returnKey.begin(), returnKey.end(), returnKey.begin(), ::tolower);
//    }
//
//    if (isArray) {
//      if (returnKey == "array") {
//        // nested arrays
//        returnKey = "[]";
//      }
//      out = returnKey + "[]";
//    }
//    else if (isDictionary) {
//      VueComponentFormatter::findAndReplaceIn(returnKey, ".Key", "");
//
//      out = "Map<" + returnKey + ", ";
//
//      string returnValue;
//
//      if (innerReturnType.size() > 1) {
//        returnValue = vuePropifyTypeName(innerReturnType[1]);
//      }
//      else {
//        const InterfaceType& interfaceType = type.as<InterfaceType>();
//        returnValue = vuePropifyTypeName(interfaceType.interface->jsName);
//      }
//
//      if (nativeTypes[returnValue]) {
//        transform(returnValue.begin(), returnValue.end(), returnValue.begin(), ::tolower);
//      }
//
//      // When using nativeTypes inside a ts def
//      // (as opposed to a vue prop def like { type: Object }),
//      // lowercase `object` is verboten
//      if (returnValue == "object") {
//        returnValue = "any";
//      }
//      if (returnValue == "id") {
//        returnValue = "any";
//      }
//
//      VueComponentFormatter::findAndReplaceIn(returnValue, ",id>", "");
//
//      out += returnValue;
//      out += ">";
//    }
//    else {
//      out = "JSManagedValue";
//    }
//  }

//  string pointerTypeStr = pointerQualType.getAsString();
//
//  if (pointerTypeStr.rfind("__kindof ", 0) == 0)
//  {
//    out = "typeof " + out;
//  }
//
//  return out;
}

string VueComponentFormatter::getFunctionProto(const vector<Type*>& signature, const clang::QualType qualType)
{
  ostringstream output;
  output << "JSManagedValue";
  return output.str();
}

string VueComponentFormatter::getTypeString(clang::ASTContext &Ctx, clang::Decl::ObjCDeclQualifier Quals, clang::QualType qualType, const Type& type, const bool isFuncParam) {
  clang::QualType pointerType = Ctx.getUnqualifiedObjCPointerType(qualType);
  
  if (pointerType.getAsString().find("instancetype ", 0) != string::npos) {
    return "Self";
  }
  
  string formattedOut = VueComponentFormatter::current.formatType(type, pointerType);
  bool isOptional = false;
  
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
  
  if (isOptional) {
    formattedOut += "?";
  }
  
  if (formattedOut == "") {
    cerr << "No type string found for " << pointerType.getAsString() << " - " << qualType.getAsString() << endl;
  }
  
  return formattedOut;
}

string VueComponentFormatter::getInstanceParamsStr(MethodMeta* method, BaseClassMeta* owner, bool forCall) {
  vector<string> argumentLabels = method->argLabels;
  
  string output = "";
  const clang::ObjCMethodDecl& methodDecl = *clang::dyn_cast<clang::ObjCMethodDecl>(method->declaration);
  
  output += "(";
  
  const auto parameters = methodDecl.parameters();
  size_t lastParamIndex = method->getFlags(::Meta::MetaFlags::MethodHasErrorOutParameter) ? (parameters.size() - 1) : parameters.size();
  
  size_t numEmptyLabels = 0;
  size_t numLabels = argumentLabels.size();
  size_t numUnLabeledArgs = lastParamIndex - numLabels;
  bool isInit = method->getFlags(MethodIsInitializer);
  bool isInitWithTargetAction = isInit && method->hasTargetAction();
  
  for (size_t i = 0; i < lastParamIndex; i++) {
    const clang::ParmVarDecl parmVar = *parameters[i];
    const clang::Decl::ObjCDeclQualifier qualifier = parmVar.getObjCDeclQualifier();
    clang::QualType qualType = parmVar.getType();
    
    string paramLabel = "";
    string paramName = "";
    
    if (!isInit && numUnLabeledArgs > numEmptyLabels) {
      paramLabel = "_";
      numEmptyLabels++;
      if (numLabels > 0) {
        if (i < argumentLabels.size()) {
          paramName = argumentLabels[i];
        }
        else if (i < parameters.size()) {
          cerr << "Warning: fell back to param label instead of argument label for " << method->name << endl;
          paramName = parameters[i]->getNameAsString();
        }
      }
    }
    else {
      auto idxToLookForName = i - numEmptyLabels;
      if (idxToLookForName < argumentLabels.size()) {
        paramName = argumentLabels[idxToLookForName];
      }
      else {
        cerr << methodDecl.getNameAsString() << " - tried to get label out of bounds!\n";
      }
      
      paramLabel = paramName;
    }
    
    if (isInitWithTargetAction && i >= numLabels - 2) {
      continue;
    }
    
    string returnType = getTypeString(parmVar.getASTContext(), qualifier, qualType, *method->signature[i+1], true);
    
    auto paramLabelParts = split2(paramLabel);
    
    output += paramLabel;
    
    output += ": ";
    
      output += returnType;
    
    if (isInitWithTargetAction) {
      if (i < lastParamIndex - 3) {
        output += ", ";
      }
    }
    else if (i < lastParamIndex - 1) {
      output += ", ";
    }
  }
  
  output += ")";
  
  return output;
}

}
