#include "TypeEntities.h"
#include "MetaEntities.h"
#include <regex>

using namespace std;

namespace Meta {
static map<string, string> bridgeNames = {
  { "float", "CGFloat" },
  { "NSString", "String" },
  { "SEL", "String" },
  { "OS_dispatch_queue", "DispatchQueue" },
  { "*", "Any" },
  { "id", "Any" },
  { "void", "Void" },
  { "BOOL", "Bool" },
  { "Class", "AnyClass" },
  { "double", "Double" },
  { "size_t", "Int32st" },
  { "NSUInteger", "Int" },
  { "unsigned int", "UInt" },
  { "uint32_t", "UInt32" },
  { "int32_t", "Int32it" },
  { "uint64_t", "UInt64" },
  { "int64_t", "Int64" },
  { "int", "Int" },
  { "CFStringRef", "CFString" },
  { "CMClockRef", "CMClock" },
  { "OpaqueCMClock", "CMClock" },
  { "NSConnection", "NSXPCConnection" }
};

map<string, string> Type::apiNotes = {};
map<string, YAML::Node> Type::attributesLookup = {};

string Type::lookupApiNotes(string type) {
  if (!apiNotes[type].empty()) {
    return apiNotes[type];
  }
  
  return type;
}

string sdkVersion = getenv("SDKVERSION");
string sdkRoot = "/Library/Developer/CommandLineTools/SDKs/MacOSX" + sdkVersion + ".sdk";
string dataRoot = getenv("DATAPATH");
string attrLookupRoot = dataRoot + "/attributes";

bool Type::populateModule(string moduleName)
{
  string apiNotesPath = sdkRoot + "/System/Library/Frameworks/" +
  moduleName + ".framework/Versions/Current/Headers/" +
  moduleName + ".apinotes";
  
  if (moduleName == "ObjectiveC") {
    apiNotesPath = "/usr/include/objc/ObjectiveC.apinotes";
  }
  
  YAML::Node notes;
  
  try {
    notes = YAML::LoadFile(apiNotesPath);
    //    cout << "Loaded API notes for " << moduleName << endl;
  } catch (...) {
    //    cout << "! Could not find apinotes for " << moduleName << endl;
  }
  
  if (notes["Name"]) {
    for (auto note: notes["Globals"]) {
      if (note["SwiftName"] && note["Name"].as<string>() != note["SwiftName"].as<string>()) {
        apiNotes[note["Name"].as<string>()] = note["SwiftName"].as<string>();
      }
    }
    for (auto note: notes["Typedefs"]) {
      if (note["SwiftName"] && note["Name"].as<string>() != note["SwiftName"].as<string>()) {
        apiNotes[note["Name"].as<string>()] = note["SwiftName"].as<string>();
      }
    }
    for (auto note: notes["Protocols"]) {
      if (note["SwiftName"] && note["Name"].as<string>() != note["SwiftName"].as<string>()) {
        apiNotes[note["Name"].as<string>()] = note["SwiftName"].as<string>();
      }
    }
    for (auto note: notes["Tags"]) {
      if (note["SwiftName"] && note["Name"].as<string>() != note["SwiftName"].as<string>()) {
        apiNotes[note["Name"].as<string>()] = note["SwiftName"].as<string>();
      }
    }
    for (auto note: notes["Typedefs"]) {
      if (note["SwiftName"] && note["Name"].as<string>() != note["SwiftName"].as<string>()) {
        apiNotes[note["Name"].as<string>()] = note["SwiftName"].as<string>();
      }
    }
    for (auto note: notes["Enumerators"]) {
      if (note["SwiftName"] && note["Name"].as<string>() != note["SwiftName"].as<string>()) {
        apiNotes[note["Name"].as<string>()] = note["SwiftName"].as<string>();
      }
    }
    for (auto note: notes["Globals"]) {
      if (note["SwiftName"] && note["Name"].as<string>() != note["SwiftName"].as<string>()) {
        apiNotes[note["Name"].as<string>()] = note["SwiftName"].as<string>();
      }
    }
    for (auto cls : notes["Classes"]) {
      if (cls["Name"] && cls["SwiftName"]) {
        apiNotes[cls["Name"].as<string>()] = cls["SwiftName"].as<string>();
      }
      
      if (cls["Name"] && cls["SwiftBridge"]) {
        apiNotes[cls["Name"].as<string>()] = cls["SwiftBridge"].as<string>();
      }
      
      for (auto method : cls["Methods"]) {
        if (cls["Name"] && method["SwiftName"] && method["Selector"]) {
          apiNotes[cls["Name"].as<string>() + "." + method["SwiftName"].as<string>()] = method["Selector"].as<string>();
          apiNotes[cls["Name"].as<string>() + "." + method["Selector"].as<string>()] = method["SwiftName"].as<string>();
        }
      }
      for (auto property : cls["Properties"]) {
        if (cls["Name"] && property["SwiftName"] && property["Name"]) {
          apiNotes[cls["Name"].as<string>() + "." + property["Name"].as<string>()] = property["SwiftName"].as<string>();
        }
      }
    }
  }
  
  populateModuleAttrs(moduleName);
  
  return true;
}

bool Type::populateModuleAttrs(string moduleName)
{
  string attributesLookupPath = attrLookupRoot + "/" + moduleName + "-AttributeList.yaml";
  YAML::Node attributesNode;
  
  try {
    attributesNode = YAML::LoadFile(attributesLookupPath);
  } catch (...) {
    cerr << "Could not find attribute list for " << moduleName << endl;
    return false;
  }
  
  for (YAML::const_iterator it = attributesNode.begin(); it != attributesNode.end(); ++it) {
    attributesLookup[it->first.as<string>()] = it->second;
  }
  
  return true;
}

string Type::nameForJSExport(const string& jsName) 
{
  if (!bridgeNames[jsName].empty()) {
    return bridgeNames[jsName];
  }
  
  return jsName;
}

void Type::findAndReplaceIn(string& str, string searchFor, string replaceBy)
{
  size_t found = str.find(searchFor);
  while (found != string::npos) {
    str.replace(found, searchFor.length(), replaceBy);
    found = str.find(searchFor, found + replaceBy.length());
  };
}

string Type::formatTypeAnonymous(const Type& type, const clang::QualType pointerType) {
  string output;
  
  output += "{ ";
  
  const vector<RecordField>& fields = type.as<AnonymousStructType>().fields;
  
  for (auto& field : fields) {
    output += field.name + ": " + nameForJSExport(formatType(*field.encoding, pointerType)) + "; ";
  }
  
  output += "}";
  
  return output;
}

void Type::stripModifiersFromPointerType(string& name) {
  std::regex re("^NSObject<(\\w+)>$");
  name = std::regex_replace(name, re, "$1");
  
  findAndReplaceIn(name, "__kindof ", "");
  findAndReplaceIn(name, "const ", "");
  findAndReplaceIn(name, "struct ", "");
  findAndReplaceIn(name, " *", "");
  findAndReplaceIn(name, "<id>", "");
  findAndReplaceIn(name, " _Nullable", "");
  findAndReplaceIn(name, " _Nonnull", "");
  findAndReplaceIn(name, " _Null_unspecified", "");
  StringUtils::rtrim(name);
}

string Type::formatTypeId(const IdType& idType, const clang::QualType pointerQualType, const bool ignorePointerType) {
  if (idType.protocols.size() == 1) {
    const ::Meta::Meta& meta = *idType.protocols[0];
    if (meta.jsName != "NSCopying") {
      return meta.jsName;
    }
  }
  
  auto typePtr = pointerQualType.getTypePtrOrNull();
  
  if (typePtr) {
    string pointeeTypeName = typePtr->getPointeeType().getAsString();
    stripModifiersFromPointerType(pointeeTypeName);
    if (ignorePointerType || pointeeTypeName == "NSArray") {
      return "Any";
    }
    return nameForJSExport(pointeeTypeName);
  }
  else {
    string pointerQualTypeName = pointerQualType.getAsString();
    stripModifiersFromPointerType(pointerQualTypeName);
    return nameForJSExport(pointerQualTypeName);
  }
}

string Type::formatTypePointer(const PointerType& pointerType, const clang::QualType pointerQualType, const bool ignorePointerType) {
  string pointerQualTypeName = pointerQualType.getAsString();
  string name = pointerQualTypeName;
  stripModifiersFromPointerType(name);
  name = renamedName(name);
  
  bool hasPointerSymbol = pointerQualTypeName.find('*') != std::string::npos;
  
  if (!hasPointerSymbol) {
    // For e.g. NSRectArray returns that are PointerType but not pointers
    // for our purposes
    return name;
  }
  
  string out;
  string unsafeType = "";
  
  if (pointerQualTypeName.substr(0, 12) == "const void *") {
    return "UnsafeRawPointer";
  }
  else if (pointerType.innerType->is(TypeVoid)) {
    return "UnsafeMutableRawPointer";
  }
  
  bool hasInnerPointer = pointerType.innerType->is(TypePointer);
  
  if (hasInnerPointer) {
    out += "UnsafeMutablePointer<";
  }
  
  bool isInnerNullable = false;
  bool isPointerClassName = false;
  auto typePtr = pointerQualType.getTypePtrOrNull();
  
  regex pointerClassName(".*Pointer$");
  if (regex_match(name, pointerClassName)) {
    isPointerClassName = true;
  }
  
  if (typePtr && !isPointerClassName) {
    bool isConst = pointerQualTypeName.substr(0, 5) == "const";
    
    if (isConst) {
      unsafeType += "UnsafePointer";
    }
    else {
      regex autoreleasingRe("^\\w+<\\w+,id>$");
      string nullableSuffix = " * _Nullable * _Nullable";
      
      if (regex_match(name, autoreleasingRe)) {
        out += "Autoreleasing";
      }
      else if (StringUtils::ends_with(pointerQualTypeName, nullableSuffix)) {
        if (StringUtils::starts_with(pointerQualTypeName, "NSArray")) {
          out += "Autoreleasing";
        }
        else if (StringUtils::starts_with(pointerQualTypeName, "NSInputStream")) {
          out += "Autoreleasing";
        }
        else if (StringUtils::starts_with(pointerQualTypeName, "NSOutputStream")) {
          out += "Autoreleasing";
        }
      }
      
      unsafeType += "UnsafeMutablePointer";
    }
    
    out += unsafeType + "<";
  }
  
  // Inside bridged generics, the rules for type names are different,
  // so we don't use nameForJSExport here
  if (pointerType.innerType->is(TypeCString)) {
    out += "UnsafeMutablePointer<UInt8>";
  }
  else if (name == "BOOL") {
    out += "ObjCBool";
  }
  else if (name == "float") {
    out += "Float";
  }
  else if (name == "Array") {
    out += "NSArray";
  }
  else if (name == "String") {
    out += "NSString";
  }
  else if (name.substr(0, 13) == "NSDictionary<") {
    out += "NSDictionary";
  }
  else {
    out += nameForJSExport(name);
  }
  
  regex nullableRegex(".* _Nullable$");
  if (regex_match(typePtr->getPointeeType().getAsString(), nullableRegex)) {
    isInnerNullable = true;
  }
  
  if (hasInnerPointer) {
    out += ">";
  }
  
  if (isInnerNullable) {
    out += "?";
  }
  
  if (typePtr && !isPointerClassName) {
    out += ">";
  }
  
  return out;
}

string Type::formatTypeInterface(const Type& type, const clang::QualType pointerQualType, const bool ignorePointerType) {
  if (type.is(TypeType::TypeBridgedInterface) && type.as<BridgedInterfaceType>().isId()) {
    return formatTypeId(IdType(), pointerQualType);
  }
  
  string pointerQualTypeStr = pointerQualType.getAsString();
  const InterfaceMeta& interface = type.is(TypeType::TypeInterface) ? *type.as<InterfaceType>().interface : *type.as<BridgedInterfaceType>().bridgedInterface;
  string interfaceName = Type::nameForJSExport(renamedName(interface.jsName));
  
  if (type.hasClosedGenerics()) {
    const InterfaceType& interfaceType = type.as<InterfaceType>();
    
    if (interfaceName == "NSArray" || interfaceName == "NSMutableArray" || interfaceName == "Array") {
      string out = "";
      out = "[";
      out += formatType(*interfaceType.typeArguments[0], pointerQualType, ignorePointerType);
      out += "]";
      return out;
    }
    
    if (interfaceName == "NSDictionary" || interfaceName == "NSMutableDictionary" || interfaceName == "Dictionary") {
      string out = "";
      out = "[";
      out += formatType(*interfaceType.typeArguments[0], pointerQualType, ignorePointerType);
      out += ": ";
      out += formatType(*interfaceType.typeArguments[1], pointerQualType, true);
      out += "]";
      return out;
    }
    
//    if (interfaceName != "Set" && interfaceType.hasClosedGenerics()) {
//      cout << interfaceName << " has closed generics" << endl;
//    }
    
    if (interfaceName == "NSLayoutAnchor") {
      return "JSValue";
    }
    
    if (interfaceName == "NSDiffableDataSourceSnapshotReference") {
      return interfaceName;
    }
    
    string typeArgName;
    
    if (interfaceType.typeArguments.empty()) {
      // for NSCandidateListTouchBarItem which is generic,
      // but doesn't have type args?
      typeArgName = "AnyObject";
    }
    else {
      typeArgName = formatType(*interfaceType.typeArguments[0], pointerQualType);
      stripModifiersFromPointerType(typeArgName);
    }
    
    string out = interfaceName + "<" + typeArgName + ">";
    
    return out;
  }
  
  if (ignorePointerType) {
    return interfaceName;
  }
  
  // No generics, so we just want the inner arg type
  // e.g. NSArray<NSAppearanceName> -> NSAppearanceName
  
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
    auto objectArgs = interfaceObjectType->getTypeArgsAsWritten();
    
    if (objectArgs.size()) {
      auto argQualType = objectArgs[0];
      string firstArg = argQualType.getAsString(); // `NSString *`
      auto innerTypePtr = argQualType.getTypePtrOrNull();
      if (innerTypePtr->isObjCObjectPointerType()) {
        auto typeArgs = innerTypePtr->getAsObjCInterfacePointerType()->getObjectType()->getTypeArgsAsWritten();
        if (typeArgs.size()) {
          firstArg = typeArgs[0].getAsString();
        }
      }
      
      stripModifiersFromPointerType(firstArg); // `NSString`
      
      // NSView & NSViewSubclass
      if (interfaceName == "NSView") {
        firstArg = bitwiseView(firstArg);
      }
      
      return Type::nameForJSExport(renamedName(firstArg)); // `String`;
    }
    else {
      string pointeeTypeName = typePtr->getPointeeType().getAsString();
      stripModifiersFromPointerType(pointeeTypeName);
      
      if (pointeeTypeName == "NSArray") {
        return "[Any]";
      }
      else if (pointeeTypeName == "NSDictionary") {
        return "[AnyHashable: Any]";
      }
      else if (interfaceName == "NSObject") {
        return Type::nameForJSExport(renamedName(pointeeTypeName));
      }
      else if (interfaceName == "NSView") {
        pointeeTypeName = bitwiseView(pointeeTypeName);
        return Type::nameForJSExport(renamedName(pointeeTypeName));
      }
      else {
        stripModifiersFromPointerType(pointerQualTypeStr);
        return nameForJSExport(renamedName(pointerQualTypeStr));
      }
    }
  }
  else {
    stripModifiersFromPointerType(pointerQualTypeStr);
    return nameForJSExport(renamedName(pointerQualTypeStr));
  }
  
  throw logic_error(string("Misparsed interface definition '") + interfaceName + "'.");
}

string Type::formatType(const Type& type, const clang::QualType pointerType, const bool ignorePointerType)
{
  string typeStr = "";
  string pointerTypeStr = pointerType.getAsString();
  
  switch (type.getType()) {
    case TypeVoid:
      typeStr = "Void";
      break;
    case TypeBool:
      typeStr = "Bool";
      break;
    case TypeSignedChar:
      typeStr = "Int8";
      break;
    case TypeUnsignedChar:
      typeStr = "UInt8";
      break;
    case TypeShort:
      typeStr = "Int16";
      break;
    case TypeUShort:
      //      typeStr = "UInt16";
      typeStr = type.nameForJSExport(::Meta::renamedName(pointerType.getAsString()));
      break;
    case TypeInt:
      typeStr = "Int32";
      break;
    case TypeUInt:
      //      typeStr = "UInt";
      typeStr = type.nameForJSExport(renamedName(pointerType.getAsString()));
      break;
    case TypeLong: {
      if (pointerTypeStr == "NSInteger") {
        typeStr = "Int";
      }
      else {
        typeStr = type.nameForJSExport(::Meta::renamedName(pointerType.getAsString()));
      }
      break;
    }
    case TypeULong: {
      if (pointerTypeStr == "NSUInteger * _Nonnull") {
        typeStr = "UnsafeMutablePointer<Int>";
      }
      else {
        typeStr = "Int";
      }
      break;
    }
    case TypeLongLong:
      typeStr = "Int64";
      break;
    case TypeULongLong:
      typeStr = "UInt64";
      break;
    case TypeFloat:
    case TypeDouble: {
      if (pointerType.getAsString() == "float") {
        return "Float";
      }
      else if (ignorePointerType || pointerType.getAsString() == "CGFloat") {
        typeStr = "CGFloat";
      }
      else {
        typeStr = nameForJSExport(pointerType.getAsString());
      }
      break;
    }
    case TypeSelector:
      typeStr = "Selector";
      break;
    case TypeUnichar:
      typeStr = "String /* TypeUnichar */ ";
      break;
    case TypeCString: {
      string pointerTypeStr = pointerType.getAsString();
      
      bool isMutable = pointerTypeStr.substr(0, 5) != "const";
      bool isUnsigned = false;
      
      if (isMutable) {
        isUnsigned = pointerTypeStr.substr(0, 8) == "unsigned";
        typeStr = "UnsafeMutablePointer<";
      }
      else {
        isUnsigned = pointerTypeStr.substr(6, 8) == "unsigned";
        typeStr = "UnsafePointer<";
      }
      
      if (isUnsigned) {
        typeStr += "UInt8";
      }
      else {
        typeStr += "Int8";
      }
      
      typeStr += ">";
      
      break;
    }
    case TypeProtocol:
      typeStr = "Protocol";
      break;
    case TypeClass: {
      auto pointeeType = pointerType->getPointeeType();
      string pointeeTypeStr = pointeeType.getAsString();
      stripModifiersFromPointerType(pointeeTypeStr);
      regex re("^\\w+<(\\w+)>$");
      string pointeeTypeInnerStr = regex_replace(pointeeTypeStr, re, "$1");
      
      if (pointeeTypeStr == "Class" || pointeeTypeInnerStr == "Class") {
        return "AnyClass";
      }
      
      typeStr = lookupApiNotes(nameForJSExport(pointeeTypeInnerStr)) + ".Type";
      break;
    }
    case TypeStruct:
      typeStr = Type::nameForJSExport(type.as<StructType>().structMeta->jsName);
      break;
    case TypeUnion:
      typeStr = Type::nameForJSExport(type.as<UnionType>().unionMeta->jsName);
      break;
    case TypeEnum:
      typeStr = nameForJSExport(type.as<EnumType>().enumMeta->jsName);
      break;
    case TypeTypeArgument:
      typeStr = type.as<TypeArgumentType>().name;
      break;
    case TypeBlock:
      typeStr = "JSValue";
      break;
    case TypeFunctionPointer:
      typeStr = "JSValue";
      break;
    case TypeConstantArray:
    case TypeExtVector: {
      string name = lookupApiNotes(formatType(*type.as<ConstantArrayType>().innerType, pointerType));
      typeStr = "UnsafeMutablePointer<" + name + ">";
      break;
    }
    case TypeIncompleteArray:
      typeStr = formatType(*type.as<IncompleteArrayType>().innerType, pointerType);
      break;
    case TypeId:
      typeStr = formatTypeId(type.as<IdType>(), pointerType, ignorePointerType);
      break;
    case TypePointer:
      typeStr = formatTypePointer(type.as<PointerType>(), pointerType, ignorePointerType);
      break;
    case TypeInterface:
    case TypeBridgedInterface:
      typeStr = formatTypeInterface(type, pointerType, ignorePointerType);
      break;
    case TypeAnonymousStruct:
    case TypeAnonymousUnion:
      typeStr = formatTypeAnonymous(type, pointerType);
      break;
    case TypeInstancetype:
      typeStr = "Self";
      break;
    case TypeVaList:
    default:
      break;
  }
  
  return lookupApiNotes(typeStr);
}

string Type::writeFunctionProto(const vector<Type*>& signature)
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

string Type::tsifyType(const Type& type, const bool isFuncParam, const bool forComponent)
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

// NSView<NSCollectionViewElement>
// to
// (NSView & NSCollectionViewElement)
string Type::bitwiseView(string& name) {
  if (name.substr(0, 7) == "NSView<") {
    name.pop_back();
    name = name.substr(7, name.size());
    auto argName = nameForJSExport(renamedName(name));
    return "(NSView & " + argName + ")";
  }
  
  return name;
}

bool Type::hasClosedGenerics() const
{
  if (this->is(TypeInterface)) {
    const InterfaceType& interfaceType = this->as<InterfaceType>();
    if (interfaceType.interface->name == "NSCandidateListTouchBarItem") {
      return true;
    }
    return interfaceType.typeArguments.size();
  }
  
  return false;
}


}
