/*
* Copyright 2018-2019 Redis Labs Ltd. and Contributors
*
* This file is available under the Apache License, Version 2.0,
* modified with the Commons Clause restriction.
*/

#include "ast.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../graph/entities/graph_entity.h"
#include "./ast_arithmetic_expression.h"
#include "../arithmetic/repository.h"
#include "../arithmetic/arithmetic_expression.h"

/* Compares a triemap of user-specified functions with the registered functions we provide. */
AST_Validation _AST_ValidateReferredFunctions(TrieMap *referred_functions,
                                              char **reason,
                                              bool include_aggregates) {
  void *value;
  tm_len_t len;
  char *funcName;
  TrieMapIterator *it = TrieMap_Iterate(referred_functions, "", 0);
  AST_Validation res = AST_VALID;
  *reason = NULL;

  // TODO: return RAX.
  while(TrieMapIterator_Next(it, &funcName, &len, &value)) {
    funcName[len] = 0;
    // No functions have a name longer than 32 characters
    if (len >= 32) {
      res = AST_INVALID;
      break;
    }

    if (AR_FuncExists(funcName)) continue;

    if (Agg_FuncExists(funcName)) {
      if (include_aggregates) {
        continue;
      } else {
        // Provide a unique error for using aggregate functions from inappropriate contexts
        asprintf(reason, "Invalid use of aggregating function '%s'", funcName);
        res = AST_INVALID;
        break;
      }
    }

    // If we reach this point, the function was not found
    res = AST_INVALID;
    break;
  }

  // If the function was not found, provide a reason if one is not set
  if (res == AST_INVALID && *reason == NULL) {
    asprintf(reason, "Unknown function '%s'", funcName);
  }

  TrieMapIterator_Free(it);

  return res;
}

AST_Query *New_AST_Query(AST_MatchNode *matchNode, AST_WhereNode *whereNode,
                         AST_CreateNode *createNode, AST_MergeNode *mergeNode,
                         AST_SetNode *setNode, AST_DeleteNode *deleteNode,
                         AST_ReturnNode *returnNode, AST_OrderNode *orderNode,
                         AST_SkipNode *skipNode, AST_LimitNode *limitNode,
                         AST_IndexNode *indexNode, AST_UnwindNode *unwindNode) {
  AST_Query *queryExpressionNode = (AST_Query *)malloc(sizeof(AST_Query));

  queryExpressionNode->matchNode = matchNode;
  queryExpressionNode->whereNode = whereNode;
  queryExpressionNode->createNode = createNode;
  queryExpressionNode->mergeNode = mergeNode;
  queryExpressionNode->setNode = setNode;
  queryExpressionNode->deleteNode = deleteNode;
  queryExpressionNode->returnNode = returnNode;
  queryExpressionNode->orderNode = orderNode;
  queryExpressionNode->skipNode = skipNode;
  queryExpressionNode->limitNode = limitNode;
  queryExpressionNode->indexNode = indexNode;
  queryExpressionNode->unwindNode = unwindNode;

  return queryExpressionNode;
}

AST_Validation _Aliases_Defined(const AST_Query *ast,
                                char **undefined_alias) {
  /* Check that all the aliases that are in aliasesToCheck exists in the match clause */
  AST_Validation res = AST_VALID;
  
  TrieMap *aliasesToCheck = NewTrieMap();
  ReturnClause_ReferredEntities(ast->returnNode, aliasesToCheck);
  DeleteClause_ReferredEntities(ast->deleteNode, aliasesToCheck);
  SetClause_ReferredEntities(ast->setNode, aliasesToCheck);
  WhereClause_ReferredEntities(ast->whereNode, aliasesToCheck);
  UnwindClause_ReferredEntities(ast->unwindNode, aliasesToCheck);

  TrieMap *definedAliases = NewTrieMap();
  MatchClause_DefinedEntities(ast->matchNode, definedAliases);
  UnwindClause_DefinedEntities(ast->unwindNode, definedAliases);
  

  TrieMapIterator *it = TrieMap_Iterate(aliasesToCheck, "", 0);
  char *alias;
  tm_len_t len;
  void *value;

  while(TrieMapIterator_Next(it, &alias, &len, &value)) {
    if(TrieMap_Find(definedAliases, alias, len) == TRIEMAP_NOTFOUND) {
      asprintf(undefined_alias, "%s not defined", alias);
      res = AST_INVALID;
      break;
    }
  }

  TrieMapIterator_Free(it);
  TrieMap_Free(aliasesToCheck, TrieMap_NOP_CB);
  TrieMap_Free(definedAliases, TrieMap_NOP_CB);
  return res;
}

AST_Validation _Validate_CREATE_Clause(const AST_Query *ast, char **reason) {
  if(!ast->createNode) return AST_VALID;
  AST_CreateNode *createNode = ast->createNode;
  
  size_t entityCount = Vector_Size(createNode->graphEntities);
  for(int i = 0; i < entityCount; i++) {
    AST_GraphEntity *entity;
    Vector_Get(createNode->graphEntities, i, &entity);
    
    if(entity->t == N_ENTITY) continue;
    if (!entity->label) {
      asprintf(reason, "Exactly one relationship type must be specified for CREATE");
      return AST_INVALID;
    }
  }

  return AST_VALID;
}

AST_Validation _Validate_DELETE_Clause(const AST_Query *ast, char **reason) {

  if (!ast->deleteNode) {
    return AST_VALID;
  }

  if (!ast->matchNode) {
    return AST_INVALID;
  }
  
  return AST_VALID;
}

AST_Validation _Validate_MATCH_Clause(const AST_Query *ast, char **reason) {
  if(!ast->matchNode) return AST_VALID;
  
  // Check to see if an edge is being reused.
  AST_Validation res = AST_VALID;
  TrieMap *edgeAliases = NewTrieMap();  // Will let us know if duplicates.

  // Scan mentioned edges.
  size_t entityCount = Vector_Size(ast->matchNode->_mergedPatterns);
  for(int i = 0; i < entityCount; i++) {
    AST_GraphEntity *entity;
    Vector_Get(ast->matchNode->_mergedPatterns, i, &entity);

    if(entity->t != N_LINK) continue;

    AST_LinkEntity *edge = (AST_LinkEntity*) entity;
    if(edge->length) {
      if(edge->length->minHops > edge->length->maxHops) {
        asprintf(reason, "Variable length path, maximum number of hops must be greater or equal to minimum number of hops.");
        res = AST_INVALID;
        break;
      }
    }

    char *alias = entity->alias;
    /* The query is validated before and after aliasing anonymous entities,
     * so alias may be NULL at this time. */
    if (!alias) continue;

    int new = TrieMap_Add(edgeAliases, alias, strlen(alias), NULL, TrieMap_DONT_CARE_REPLACE);
    if(!new) {
      asprintf(reason, "Cannot use the same relationship variable '%s' for multiple patterns.", alias);
      res = AST_INVALID;
      break;
    }
  }

  TrieMap_Free(edgeAliases, TrieMap_NOP_CB);

  /* Verify that no alias appears in multiple independent patterns.
   * TODO We should introduce support for this when possible. */
  int patternCount = Vector_Size(ast->matchNode->patterns);
  if (Vector_Size(ast->matchNode->patterns) > 1) {
    TrieMap *reused_entities = NewTrieMap();
    Vector *pattern;
    AST_GraphEntity *elem;

    int pattern_ids[patternCount];
    for (int i = 0; i < patternCount; i ++) {
      pattern_ids[i] = i;
      Vector_Get(ast->matchNode->patterns, i, &pattern);
      for (int j = 0; j < Vector_Size(pattern); j ++) {
        Vector_Get(pattern, j, &elem);
        char *alias = elem->alias;
        if (!alias) continue;
        void *val = TrieMap_Find(reused_entities, alias, strlen(alias));
        // If this alias has been used in a previous pattern, emit an error.
        if (val != TRIEMAP_NOTFOUND && *(int*)val != i) {
          asprintf(reason, "Alias '%s' reused. Entities with the same alias may not be referenced in multiple patterns.", alias);
          res = AST_INVALID;
          break;
        }
        // Use the pattern index as a value, as it is valid to reuse a node alias within a pattern.
        TrieMap_Add(reused_entities, alias, strlen(alias), &pattern_ids[i], TrieMap_DONT_CARE_REPLACE);
      }
    }
    TrieMap_Free(reused_entities, TrieMap_NOP_CB);
  }

  return res;
}

AST_Validation _Validate_RETURN_Clause(const AST_Query *ast, char **reason) {
  char *undefined_alias;

  if (!ast->returnNode) {
    return AST_VALID;
  }

  // Retrieve all user-specified functions in RETURN clause.
  TrieMap *ref_functions = NewTrieMap();
  ReturnClause_ReferredFunctions(ast->returnNode, ref_functions);

  // Verify that referred functions exist.
  AST_Validation res = _AST_ValidateReferredFunctions(ref_functions, reason, true);
  TrieMap_Free(ref_functions, TrieMap_NOP_CB);

  return res;
}

AST_Validation _Validate_SET_Clause(const AST_Query *ast, char **reason) {
  char *undefined_alias;

  if (!ast->setNode) {
    return AST_VALID;
  }

  if (!ast->matchNode) {
    return AST_INVALID;
  }

  return AST_VALID;
}

AST_Validation _Validate_WHERE_Clause(const AST_Query *ast, char **reason) {
  if (!ast->whereNode) return AST_VALID;
  if (!ast->matchNode) return AST_INVALID;

  // Retrieve all user-specified functions in WHERE clause.
  TrieMap *ref_functions = NewTrieMap();
  WhereClause_ReferredFunctions(ast->whereNode->filters, ref_functions);

  // Verify that referred functions exist.
  AST_Validation res = _AST_ValidateReferredFunctions(ref_functions, reason, false);

  TrieMap_Free(ref_functions, TrieMap_NOP_CB);

  return res;
}

AST_Validation AST_Validate(const AST_Query *ast, char **reason) {
  /* AST must include either a MATCH or CREATE clause. */
  if (!(ast->matchNode || ast->createNode || ast->mergeNode || ast->returnNode || ast->indexNode)) {
    asprintf(reason, "Query must specify either MATCH or CREATE clause");
    return AST_INVALID;
  }

  /* MATCH clause must be followed by either a CREATE or a RETURN clause. */
  if (ast->matchNode && !(ast->createNode || ast->returnNode || ast->setNode || ast->deleteNode || ast->mergeNode)) {
    asprintf(reason, "Query cannot conclude with MATCH (must be RETURN or an update clause)");
    return AST_INVALID;
  }

  if(_Aliases_Defined(ast, reason) != AST_VALID) {
    return AST_INVALID;
  }

  if (_Validate_MATCH_Clause(ast, reason) != AST_VALID) {
    return AST_INVALID;
  }

  if (_Validate_WHERE_Clause(ast, reason) != AST_VALID) {
    return AST_INVALID;
  }

  if (_Validate_CREATE_Clause(ast, reason) != AST_VALID) {
    return AST_INVALID;
  }

  if (_Validate_SET_Clause(ast, reason) != AST_VALID) {
    return AST_INVALID;
  }

  if (_Validate_DELETE_Clause(ast, reason) != AST_VALID) {
    return AST_INVALID;
  }

  if (_Validate_RETURN_Clause(ast, reason) != AST_VALID) {
    return AST_INVALID;
  }

  return AST_VALID;
}

void AST_NameAnonymousNodes(AST_Query *ast) {
  int entity_id = 0;
  
  if(ast->matchNode)
    MatchClause_NameAnonymousNodes(ast->matchNode, &entity_id);

  if(ast->createNode)
    CreateClause_NameAnonymousNodes(ast->createNode, &entity_id);
}

bool AST_ReadOnly(const AST_Query *ast) {
  return !(ast->createNode != NULL ||
           ast->mergeNode != NULL ||
           ast->deleteNode != NULL ||
           ast->setNode != NULL ||
           ast->indexNode != NULL);
}

void Free_AST_Query(AST_Query *queryExpressionNode) {
  Free_AST_MatchNode(queryExpressionNode->matchNode);
  Free_AST_CreateNode(queryExpressionNode->createNode);
  Free_AST_MergeNode(queryExpressionNode->mergeNode);
  Free_AST_DeleteNode(queryExpressionNode->deleteNode);
  Free_AST_SetNode(queryExpressionNode->setNode);
  Free_AST_WhereNode(queryExpressionNode->whereNode);
  Free_AST_ReturnNode(queryExpressionNode->returnNode);
  Free_AST_SkipNode(queryExpressionNode->skipNode);
  Free_AST_OrderNode(queryExpressionNode->orderNode);
  Free_AST_UnwindNode(queryExpressionNode->unwindNode);
  free(queryExpressionNode);
}
