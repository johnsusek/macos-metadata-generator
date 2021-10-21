#include "HandleMethodsAndPropertiesWithSameNameFilter.h"

namespace Meta {
using namespace std;

typedef  unordered_map<string, vector<MethodMeta*>>  MethodsStructure;

bool addMeta(MethodMeta* meta, MethodsStructure* methods, bool forceIfNameCollision)
{
    pair<unordered_map<string, vector<MethodMeta*> >::iterator, bool> insertionResult = methods->emplace(meta->jsName + to_string(meta->signature.size()), vector<MethodMeta*>());
    
    if (insertionResult.second || forceIfNameCollision) {
        vector<MethodMeta*>& metasWithSameJsName = insertionResult.first->second;
        metasWithSameJsName.push_back(meta);
        return true;
    }
    return false;
}
    
HandleMethodsAndPropertiesWithSameNameFilter::HandleMethodsAndPropertiesWithSameNameFilter(MetaFactory& metaFactory)
    : m_metaFactory(metaFactory)
{
}

bool isAllUpper(std::string& s) {
  return std::all_of(s.begin(), s.end(), [](unsigned char c){ return std::isupper(c); });
}

void HandleMethodsAndPropertiesWithSameNameFilter::filter(list<Meta*>& container)
{
    for (Meta* meta : container) {
        if (meta->is(MetaType::Interface)) {
            InterfaceMeta* interface = static_cast<InterfaceMeta*>(meta);
            
            const clang::ObjCInterfaceDecl* decl = clang::cast<clang::ObjCInterfaceDecl>(meta->declaration);

            for (clang::ObjCPropertyDecl* propertyDecl : decl->properties()) {
                if (clang::ObjCInterfaceDecl* parent = decl->getSuperClass()) {
                    clang::ObjCMethodDecl* duplicate = parent->lookupInstanceMethod(propertyDecl->getGetterName());
                    replaceMethodWithPropertyIfNecessary(duplicate, propertyDecl);
                }

                for (clang::ObjCProtocolDecl* protocol : decl->protocols()) {
                    clang::ObjCMethodDecl* duplicate = protocol->lookupInstanceMethod(propertyDecl->getGetterName());
                    replaceMethodWithPropertyIfNecessary(duplicate, propertyDecl);
                }
            }

            if (clang::ObjCInterfaceDecl* parent_decl = decl->getSuperClass()) {
                for (clang::ObjCMethodDecl* methodDecl : decl->methods()) {
                    if (!(methodDecl->isClassMethod() && !methodDecl->isPropertyAccessor())) {
                        continue;
                    }

                    if (parent_decl->lookupPropertyAccessor(methodDecl->getSelector(), nullptr, true /*IsClassProperty*/)) {
                        deleteStaticMethod(methodDecl, decl);
                    }
                }
            }
            
            MethodsStructure methods;
            for (MethodMeta* method : interface->instanceMethods) {
                addMeta(method, &methods, true);
            }
            
            // resolve collisions
            
            for (auto bucketIt = methods.begin(); bucketIt != methods.end(); ++bucketIt) {
                vector<MethodMeta*>& metas = bucketIt->second;
                if (metas.size() > 1) {
                    for (vector<Meta*>::size_type i = 0; i < metas.size(); i++) {
                      string originalJsName = metas[i]->jsName;
                      string suffix = "";
                    
                      for (auto argLabel : metas[i]->argLabels) {
                        if (argLabel == "_") {
                          continue;
                        }
                        
                        // URL -> url
                        if (isAllUpper(argLabel)) {
                          std::transform(argLabel.begin(), argLabel.end(), argLabel.begin(), ::tolower);
                        }

                        // url -> Url
                        argLabel[0] = toupper(argLabel[0]);
                        
                        suffix += argLabel;
                      }
                    
//                      metas[i]->jsName = MetaFactory::renameMeta(metas[i]->type, originalJsName, i);
//                      cout << "Write IMPL for same-name swift fn: " << metas[i]->jsName << suffix << endl;
//                      metas[i]->jsName += suffix;
                    }
                }
            }
            
        }
    }
}

/*

## Before
@protocol MyProtocol
- (int)myProperty;
@end

@interface MyInterface <MyProtocol>
@property int myProperty;
@end

## After
@protocol MyProtocol
@property int myProperty;
@end

@interface MyInterface <MyProtocol>
@property int myProperty;
@end

*/
void HandleMethodsAndPropertiesWithSameNameFilter::replaceMethodWithPropertyIfNecessary(clang::ObjCMethodDecl* duplicateMethod, clang::ObjCPropertyDecl* propertyDecl)
{
    if (duplicateMethod && !duplicateMethod->isPropertyAccessor()) {
        clang::Decl* parent_decl = clang::dyn_cast<clang::Decl>(duplicateMethod->getParent());

        Cache& cache = this->m_metaFactory.getCache();
        auto cachedMetaIt = cache.find(parent_decl);
        auto cachedMethodIt = cache.find(duplicateMethod);
        if (cachedMetaIt != cache.end() && cachedMethodIt != cache.end()) {
            BaseClassMeta* parent_meta = static_cast<BaseClassMeta*>(cachedMetaIt->second.first.get());
            MethodMeta* duplicated_method = static_cast<MethodMeta*>(cachedMethodIt->second.first.get());

            vector<MethodMeta*>& instanceMethods = parent_meta->instanceMethods;
            auto instanceMethod = find(instanceMethods.begin(), instanceMethods.end(), duplicated_method);
            if (instanceMethod != instanceMethods.end()) {
                instanceMethods.erase(instanceMethod);

                clang::SourceRange parentSourceRange = parent_decl->getSourceRange();
                clang::ObjCPropertyDecl* property_decl = clang::ObjCPropertyDecl::Create(parent_decl->getASTContext(), duplicateMethod->getDeclContext(), duplicateMethod->getSourceRange().getBegin(), duplicateMethod->getSelector().getIdentifierInfoForSlot(0), parentSourceRange.getEnd(), parentSourceRange.getBegin(), duplicateMethod->getReturnType(), duplicateMethod->getReturnTypeSourceInfo());
                property_decl->setGetterMethodDecl(duplicateMethod);

                PropertyMeta* property_meta = static_cast<PropertyMeta*>(this->m_metaFactory.create(*property_decl));
                parent_meta->instanceProperties.push_back(property_meta);
            }
        }
    }
}

/*
 
## Before
@interface MyInterface
@property (class) int myProperty;
@end

@interface MyDerivedInterface : MyInterface
+ (int)myProperty;
@end
 
## After
@interface MyInterface
@property (class) int myProperty;
@end

@interface MyDerivedInterface : MyInterface
@end

*/
void HandleMethodsAndPropertiesWithSameNameFilter::deleteStaticMethod(const clang::ObjCMethodDecl* duplicateMethod, const clang::ObjCInterfaceDecl* owner)
{
    Cache& cache = this->m_metaFactory.getCache();
    auto cachedMetaIt = cache.find(owner);
    auto cachedMethodIt = cache.find(duplicateMethod);
    if (cachedMetaIt != cache.end() && cachedMethodIt != cache.end()) {
        BaseClassMeta* parent_meta = static_cast<BaseClassMeta*>(cachedMetaIt->second.first.get());
        MethodMeta* duplicated_method = static_cast<MethodMeta*>(cachedMethodIt->second.first.get());

        vector<MethodMeta*>& staticMethods = parent_meta->staticMethods;
        auto staticMethod = find(staticMethods.begin(), staticMethods.end(), duplicated_method);
        if (staticMethod != staticMethods.end()) {
            staticMethods.erase(staticMethod);
        }
    }
}
}
