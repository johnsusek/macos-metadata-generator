#pragma once

#include "MetaVisitor.h"
#include "TypeEntities.h"
#include "Utils/Noncopyable.h"
#include "Utils/StringUtils.h"
#include <clang/Basic/Module.h>
#include <clang/AST/DeclBase.h>
#include <clang/AST/DeclObjC.h>
#include <iostream>
#include <llvm/ADT/iterator_range.h>
#include <map>
#include <unordered_set>
#include <string>
#include <unordered_map>
#include <vector>
#include <regex>

#define UNKNOWN_VERSION \
    {                   \
        -1, -1, -1      \
    }

namespace Meta {

std::string renamedName(std::string name, std::string ownerKey = "");
std::string sanitizeIdentifierForSwift(const std::string& identifierName);
std::string getFunctionProtoCall(std::string paramName, const std::vector<::Meta::Type*>& signature, const clang::QualType qualType);
std::string getFunctionInterfaceCall(std::string paramName, const ::Meta::InterfaceType& interface, const clang::QualType qualType);
std::string jsConversionFnName(std::string paramName, const ::Meta::Type& type, const clang::QualType qualType);

static std::unordered_set<std::string> nonNullable;

enum ParamCallType {
  Definition = 0, // initWithFrame(_: CGRect)
  Implementation, // initWithFrame(_ frame: CGRect)
  Call // initWithFrame(frame)
};

struct Version {
    static Version Unknown;
    
    int Major;
    int Minor;
    int SubMinor;
    
    bool isUnknown() const {
        return this->Major <= 0;
    }
      
    bool isGreaterThanOrUnknown(const Version& other) const {
      return this->isUnknown() ||  (*this > other && !other.isUnknown());
    }
    
    bool isGreaterThanOrEqualOrUnknown(const Version& other) const {
      return this->isUnknown() ||  (*this >= other && !other.isUnknown());
    }
    
    bool operator ==(const Version& other) const {
        return this->Major == other.Major &&
            this->Minor == other.Minor && this->SubMinor == other.SubMinor;
    }

    bool operator !=(const Version& other) const {
        return !(*this == other);
    }
    
    bool operator <(const Version& other) const {
        return this->Major < other.Major ||
                (this->Major == other.Major &&
                    (this->Minor < other.Minor ||
                     (this->Minor == other.Minor && this->SubMinor < other.SubMinor)
                    )
                );
    }

    bool operator <=(const Version& other) const {
        return *this == other || *this < other;
    }

    bool operator >(const Version& other) const {
        return !(*this <= other);
    }

    bool operator >=(const Version& other) const {
        return !(*this < other);
    }

    std::string to_string() const {
      std::string result = "";

      if (this->Major != -1) {
        result += std::to_string(this->Major);
      }
      if (this->Minor != -1) {
        result += "." + std::to_string(this->Minor);
      }
      if (this->SubMinor != -1) {
        result += "." + std::to_string(this->SubMinor);
      }

      return result;
    }
};

enum MetaFlags : uint16_t {
    // Common
    None = 0,
    IsIosAppExtensionAvailable = 1 << 0,
    // Function
    FunctionIsVariadic = 1 << 1,
    FunctionOwnsReturnedCocoaObject = 1 << 2,
    FunctionReturnsUnmanaged = 1 << 3,
    // Method
    MethodIsVariadic = 1 << 4,
    MethodIsNullTerminatedVariadic = 1 << 5,
    MethodOwnsReturnedCocoaObject = 1 << 6,
    MethodHasErrorOutParameter = 1 << 7,
    MethodIsInitializer = 1 << 8,
    MethodReturnsSelf = 1 << 9,
  
    // Member
    MemberIsOptional = 1 << 10,
};

enum MetaType {
    Undefined = 0,
    Struct,
    Union,
    Function,
    Enum,
    Var,
    Interface,
    Protocol,
    Category,
    Method,
    Property,
    EnumConstant
};

class Meta {
public:
    MetaType type = MetaType::Undefined;
    MetaFlags flags = MetaFlags::None;

    std::string name;
    std::string demangledName;
    std::string jsName;
    std::vector<std::string> argLabels;
    bool isRenamed;
    std::string fileName;
    bool includeSelector = false;
    clang::Module* module = nullptr;
    const clang::Decl* declaration = nullptr;

    // Availability
    bool unavailable = false;
    Version introducedIn = UNKNOWN_VERSION;
    Version obsoletedIn = UNKNOWN_VERSION;
    Version deprecatedIn = UNKNOWN_VERSION;

    Meta() = default;
    virtual ~Meta() = default;

    // visitors
    virtual void visit(MetaVisitor* serializer) = 0;

    bool is(MetaType type) const
    {
        return this->type == type;
    }

    bool getFlags(MetaFlags flags) const
    {
        return (this->flags & flags) == flags;
    }

    bool hasParamOfType(std::string typeStr) const
    {
      const clang::ObjCMethodDecl& methodDecl = *clang::dyn_cast<clang::ObjCMethodDecl>(this->declaration);
      const auto parameters = methodDecl.parameters();
      size_t lastParamIndex = this->getFlags(::Meta::MetaFlags::MethodHasErrorOutParameter) ? (parameters.size() - 1) : parameters.size();
      
      for (size_t i = 0; i < lastParamIndex; i++) {
        clang::ParmVarDecl parmVar = *parameters[i];
        clang::QualType qualType = parmVar.getType();
        std::string paramTypeStr = qualType.getAsString();
        
        if (paramTypeStr.substr(0, typeStr.size()) == typeStr) {
          return true;
        }
      }
      
      return false;
    }
  
    bool isInit() const
    {
      bool isInit = this->getFlags(MethodIsInitializer)
        || this->getFlags(MethodReturnsSelf)
        || this->jsName == "create";

      return isInit;
    }

    void setFlags(MetaFlags flags, bool value)
    {
        if (value) {
            this->flags = static_cast<MetaFlags>(this->flags | flags);
        } else {
            this->flags = static_cast<MetaFlags>(this->flags & ~flags);
        }
    }
    
    bool getUnavailableInSwift(Meta* owner);
    std::string dumpDeclComments(Meta* owner);

    template <class T>
    const T& as() const
    {
        return *static_cast<const T*>(this);
    }

    template <class T>
    T& as()
    {
        return *static_cast<T*>(this);
    }

    std::string availableString() const
    {
      std::string result = "";
      
      if (introducedIn.Major > 0) {
        result += "@available(OSX ";
        
        result += introducedIn.to_string();
        
        result += ", *) ";
      }
      return result;
    }

    std::string identificationString() const
    {
        return std::string("[Name: '") + name + "', JsName: '" + jsName + "', Module: '" + ((module == nullptr) ? "" : module->getFullModuleName()) + "', File: '" + fileName + "']";
    }
  
    std::string shortName() const
    {
      if (this->jsName == "IKImageView") {
        return this->jsName;
      }
      std::regex frameworkPrefixes("^(NS|AV|IK)");
      return regex_replace(this->jsName, frameworkPrefixes, "");
    }
  
    std::string kebabCase(std::string camelCase) const {
      std::string str(1, tolower(camelCase[0]));

      for (auto it = camelCase.begin() + 1; it != camelCase.end(); ++it) {
        if (isupper(*it) && *(it-1) != '-' && islower(*(it-1))) {
          str += "-";
        }
        str += *it;
      }

      std::transform(str.begin(), str.end(), str.begin(), ::tolower);

      return str;
    }

    std::string kebabName() const
    {
      std::regex re1("^(NS|AV|IK)");
      std::string noPrefix = this->kebabCase(std::regex_replace(this->jsName, re1, ""));
      
      if (noPrefix == "text-view") {
        return noPrefix;
      }
      
      std::regex re2("-view$");
      std::string noSuffix = std::regex_replace(noPrefix, re2, "");
      
      return noSuffix;
    }
};

class BaseClassMeta : public Meta {
public:
  std::vector<MethodMeta*> instanceMethods;
  std::vector<MethodMeta*> staticMethods;
  std::vector<PropertyMeta*> instanceProperties;
  std::vector<PropertyMeta*> staticProperties;
  std::vector<ProtocolMeta*> protocols;
};

class MethodMeta : public Meta {
public:
    MethodMeta()
        : Meta()
    {
        this->type = MetaType::Method;
    }
  // just a more convenient way to get the selector of method
  std::string getSelector() const
  {
    return this->name;
  }
  
  // Get the replacement selector name from api notes and attrs
  std::string getReplacement();
  std::string builtName();
  std::string getParamsAsString(BaseClassMeta* owner, ParamCallType callType = ParamCallType::Definition);
  std::string getTypeNullability(clang::ParmVarDecl* decl);
  std::string getTypeNullability(BaseClassMeta* owner);
  bool hasTargetAction();

  std::vector<Type*> signature;
  std::vector<std::string> constructorTokens;

  virtual void visit(MetaVisitor* visitor) override;
};

class PropertyMeta : public Meta {
public:
    PropertyMeta()
        : Meta()
    {
        this->type = MetaType::Property;
    }

    MethodMeta* getter = nullptr;
    MethodMeta* setter = nullptr;

    std::string getNullabilitySymbol(BaseClassMeta* owner);

    virtual void visit(MetaVisitor* visitor) override;
};

class ProtocolMeta : public BaseClassMeta {
public:
    ProtocolMeta()
    {
        this->type = MetaType::Protocol;
    }

    virtual void visit(MetaVisitor* visitor) override;
};

class CategoryMeta : public BaseClassMeta {
public:
    CategoryMeta()
    {
        this->type = MetaType::Category;
    }

    InterfaceMeta* extendedInterface;

    virtual void visit(MetaVisitor* visitor) override;
};

class InterfaceMeta : public BaseClassMeta {
public:
    InterfaceMeta()
    {
        this->type = MetaType::Interface;
    }

    InterfaceMeta* base;

    virtual void visit(MetaVisitor* visitor) override;

    bool isSubclassOf(std::string superclass);
  
    bool nameExistsInSuperclass(std::string name, MetaType metaType) {
      bool stop = false;
      bool isInSuperclass = false;
      auto base = this->base;
      
      while (stop == false) {
        if (base && base != NULL && base != nullptr) {
          if (metaType == Method) {
            for (MethodMeta* baseMethod : base->instanceMethods) {
              if (name == baseMethod->jsName) {
                return true;
              }
            }
            for (MethodMeta* baseMethod : base->staticMethods) {
              if (name == baseMethod->jsName) {
                return true;
              }
            }
          }
          
          if (metaType == Property) {
            for (PropertyMeta* baseProperty : base->instanceProperties) {
              if (name == baseProperty->jsName) {
                return true;
              }
            }
            for (PropertyMeta* baseProperty : base->staticProperties) {
              if (name == baseProperty->jsName) {
                return true;
              }
            }
          }
          
          if (!stop) {
            base = base->base;
          }
        }
        else {
          stop = true;
        }
      }
      
      return isInSuperclass;
    }
};

class RecordMeta : public Meta {
public:
    std::vector<RecordField> fields;
};

class StructMeta : public RecordMeta {
public:
    StructMeta()
    {
        this->type = MetaType::Struct;
    }

    virtual void visit(MetaVisitor* visitor) override;
};

class UnionMeta : public RecordMeta {
public:
    UnionMeta()
    {
        this->type = MetaType::Union;
    }

    virtual void visit(MetaVisitor* visitor) override;
};

class FunctionMeta : public Meta {
public:
    FunctionMeta()
    {
        this->type = MetaType::Function;
    }
    std::vector<Type*> signature;

    virtual void visit(MetaVisitor* visitor) override;
};

class EnumConstantMeta : public Meta {
public:
    EnumConstantMeta()
    {
        this->type = MetaType::EnumConstant;
    }

    std::string value;

    bool isScoped = false;

    virtual void visit(MetaVisitor* visitor) override;
};

struct EnumField {
    std::string name;
    std::string value;
};

class EnumMeta : public Meta {
public:
    EnumMeta()
    {
        this->type = MetaType::Enum;
    }

    std::vector<EnumField> fullNameFields;

    std::vector<EnumField> swiftNameFields;

    virtual void visit(MetaVisitor* visitor) override;
};

class VarMeta : public Meta {
public:
    VarMeta()
    {
        this->type = MetaType::Var;
    }

    Type* signature = nullptr;
    bool hasValue = false;
    std::string value;

    virtual void visit(MetaVisitor* visitor) override;
};
}
