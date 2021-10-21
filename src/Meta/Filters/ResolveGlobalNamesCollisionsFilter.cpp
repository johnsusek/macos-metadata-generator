//
// Created by Ivan Buhov on 9/6/15.
//

#include "ResolveGlobalNamesCollisionsFilter.h"
#include "Meta/MetaFactory.h"

namespace Meta {
using namespace std;

static int getPriority(Meta* meta)
{
    switch (meta->type) {
    case MetaType::Interface:
        return 8;
    case MetaType::Protocol:
        return 7;
    case MetaType::Function:
        return 6;
    case MetaType::Var:
        return 5;
    case MetaType::Struct:
        return 4;
    case MetaType::Union:
        return 3;
    case MetaType::Enum:
        return 2;
    case MetaType::EnumConstant:
        return 1;
    default:
        return 0;
    }
}

static bool metasComparerByPriority(Meta* meta1, Meta* meta2)
{
    return getPriority(meta1) > getPriority(meta2);
}
void ResolveGlobalNamesCollisionsFilter::filter(list<Meta*>& container)
{

    // order meta objects by modules and names
    for (Meta* meta : container) {
        addMeta(meta, true);
    }

    // resolve collisions
    vector<Meta*> conflictingMetas;
    for (auto modulesIt = _modules.begin(); modulesIt != _modules.end(); ++modulesIt) {
        for (auto bucketIt = modulesIt->second.begin(); bucketIt != modulesIt->second.end(); ++bucketIt) {
            vector<Meta*>& metas = bucketIt->second;
            if (metas.size() > 1) {
                sort(metas.begin(), metas.end(), metasComparerByPriority);
                for (vector<Meta*>::size_type i = 1; i < metas.size(); i++) {
                    conflictingMetas.push_back(metas[i]);
                }
                metas.resize(1); // leave only the meta with the highest priority in the bucket
            }
        }
    }

    for (Meta* meta : conflictingMetas) {
        int index = 1;
        string originalJsName = meta->jsName;
        
        do {
            meta->jsName = MetaFactory::renameMeta(meta->type, originalJsName, index);
            index++;
        } while (!addMeta(meta, false));
    }
}

bool ResolveGlobalNamesCollisionsFilter::addMeta(Meta* meta, bool forceIfNameCollision)
{
    pair<ModulesStructure::iterator, bool> insertionResult1 = _modules.emplace(meta->module->getTopLevelModule(), unordered_map<string, vector<Meta*> >());
    unordered_map<string, vector<Meta*> >& moduleGlobalTable = insertionResult1.first->second;
    pair<unordered_map<string, vector<Meta*> >::iterator, bool> insertionResult2 = moduleGlobalTable.emplace(meta->jsName, vector<Meta*>());
    if (insertionResult2.second || forceIfNameCollision) {
        vector<Meta*>& metasWithSameJsName = insertionResult2.first->second;
        metasWithSameJsName.push_back(meta);
        return true;
    }
    return false;
}
}
