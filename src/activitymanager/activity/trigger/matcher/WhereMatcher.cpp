// Copyright (c) 2009-2018 LG Electronics, Inc.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
// http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.
//
// SPDX-License-Identifier: Apache-2.0

#include "WhereMatcher.h"

#include <stdexcept>

#include "util/Logging.h"

WhereMatcher::WhereMatcher(const MojObject& where)
        : m_where(where)
{
    validateClauses(m_where);
}

WhereMatcher::~WhereMatcher()
{
}

bool WhereMatcher::match(const MojObject& response)
{
    LOG_AM_TRACE("Entering function %s", __FUNCTION__);

    MatchResult result = checkClause(m_where, response, AndMode);
    if (result == Matched) {
        LOG_AM_DEBUG("Where Matcher: Response %s matches", MojoObjectJson(response).c_str());
        return true;
    } else {
        LOG_AM_DEBUG("Where Matcher: Response %s does not match", MojoObjectJson(response).c_str());
        return false;
    }
}

MojErr WhereMatcher::toJson(MojObject& rep, unsigned long flags) const
{
    MojErr err;

    err = rep.put(_T("where"), m_where);
    MojErrCheck(err);

    return MojErrNone;
}


void WhereMatcher::validateKey(const MojObject& key) const
{
    if (key.type() == MojObject::TypeArray) {
        for (MojObject::ConstArrayIterator iter = key.arrayBegin();
                iter != key.arrayEnd(); ++iter) {
            const MojObject& keyObj = *iter;
            if (keyObj.type() != MojObject::TypeString) {
                throw std::runtime_error(
                        "Something other than a string found in the key array of property names");
            }
        }
    } else if (key.type() != MojObject::TypeString) {
        throw std::runtime_error(
                "Property keys must be specified as a property name, or array of property names");
    }
}

void WhereMatcher::validateOp(const MojObject& op, const MojObject& val) const
{
    MojString opStr;
    MojErr err;

    if (op.type() != MojObject::TypeString) {
        throw std::runtime_error("Operation must be specified as a string property");
    }

    err = op.stringValue(opStr);
    if (err) {
        throw std::runtime_error("Failed to convert operation to string value");
    }

    if ((opStr != "<") && (opStr != "<=") && (opStr != "=") &&
            (opStr != ">=") && (opStr != ">") && (opStr != "!=") &&
            (opStr != "where")) {
        throw std::runtime_error(
                "Operation must be one of '<', '<=', '=', '>=', '>', '!=', and 'where'");
    }

    if (opStr == "where") {
        validateClauses(val);
    }
}

void WhereMatcher::validateClause(const MojObject& clause) const
{
    MojObject andClauses;
    MojObject orClauses;
    MojObject prop;
    MojObject val;
    MojObject op;
    bool found = false;

    LOG_AM_TRACE("Entering function %s", __FUNCTION__);
    LOG_AM_DEBUG("Validating where clause \"%s\"", MojoObjectJson(clause).c_str());

    if (clause.contains(_T("and"))) {
        found = true;

        clause.get(_T("and"), andClauses);
        validateClauses(andClauses);
    }

    if (clause.contains(_T("or"))) {
        if (found) {
            throw std::runtime_error("Only one of \"and\", \"or\", or a valid "
                    "clause including \"prop\", \"op\", and a \"val\"ue to "
                    "compare against must be present in a clause");
        }

        found = true;

        clause.get(_T("or"), orClauses);
        validateClauses(orClauses);
    }

    if (!clause.contains(_T("prop"))) {
        if (!found) {
            throw std::runtime_error("Each where clause must contain \"or\", "
                    "\"and\", or a \"prop\"erty to compare against");
        } else {
            return;
        }
    } else if (found) {
        throw std::runtime_error("Only one of \"and\", \"or\", or a valid "
                "clause including \"prop\", \"op\", and a \"val\"ue to "
                "compare against must be present in a clause");
    }

    clause.get(_T("prop"), prop);
    validateKey(prop);

    if (!clause.contains(_T("val"))) {
        throw std::runtime_error("Each where clause must contain a value to test against");
    }

    clause.get(_T("val"), val);

    if (!clause.contains(_T("op"))) {
        throw std::runtime_error("Each where clause must contain a test operation to perform");
    }

    clause.get(_T("op"), op);
    validateOp(op, val);
}

void WhereMatcher::validateClauses(const MojObject& where) const
{
    LOG_AM_TRACE("Entering function %s", __FUNCTION__);
    LOG_AM_DEBUG("Validating trigger clauses");

    if (where.type() == MojObject::TypeObject) {
        validateClause(where);
    } else if (where.type() == MojObject::TypeArray) {
        for (MojObject::ConstArrayIterator iter = where.arrayBegin();
                iter != where.arrayEnd(); ++iter) {
            const MojObject& clause = *iter;
            if (clause.type() != MojObject::TypeObject) {
                throw std::runtime_error("where statement array must consist of valid clauses");
            } else {
                validateClause(clause);
            }
        }
    } else {
        throw std::runtime_error(
                "where statement should consist of a single clause or array of valid clauses");
    }
}

WhereMatcher::MatchResult WhereMatcher::checkClauses(
        const MojObject& clauses, const MojObject& response, MatchMode mode) const
{
    LOG_AM_TRACE("Entering function %s", __FUNCTION__);

    if (clauses.type() == MojObject::TypeObject) {
        return checkClause(clauses, response, mode);
    } else if (clauses.type() != MojObject::TypeArray) {
        throw std::runtime_error("Multiple clauses must be specified as an array of clauses");
    }

    LOG_AM_DEBUG("Checking clauses '%s' against response '%s' (%s)",
                 MojoObjectJson(clauses).c_str(),
                 MojoObjectJson(response).c_str(),
                 (mode == AndMode) ? "and" : "or");

    for (MojObject::ConstArrayIterator iter = clauses.arrayBegin();
            iter != clauses.arrayEnd() ; ++iter) {
        MatchResult result = checkClause(*iter, response, mode);

        if (mode == AndMode) {
            if (result != Matched) {
                return NotMatched;
            }
        } else {
            if (result == Matched) {
                return Matched;
            }
        }
    }

    if (mode == AndMode) {
        return Matched;
    } else {
        return NotMatched;
    }
}

WhereMatcher::MatchResult WhereMatcher::checkClause(const MojObject& clause,
                                                          const MojObject& response,
                                                          MatchMode mode) const
{
    MojObject andClause;
    MojObject orClause;
    MojObject prop;
    MojObject op;
    MojObject val;
    MatchResult result;

    LOG_AM_TRACE("Entering function %s", __FUNCTION__);

    if (clause.type() == MojObject::TypeArray) {
        return checkClauses(clause, response, mode);
    } else if (clause.type() != MojObject::TypeObject) {
        throw std::runtime_error("Clauses must be either an object or array of objects");
    }

    LOG_AM_DEBUG("Checking clause '%s' against response '%s' (%s)",
                 MojoObjectJson(clause).c_str(),
                 MojoObjectJson(response).c_str(),
                 (mode == AndMode) ? "and" : "or");

    if (clause.contains(_T("and"))) {
        clause.get(_T("and"), andClause);

        return checkClause(andClause, response, AndMode);
    } else if (clause.contains(_T("or"))) {
        clause.get(_T("or"), orClause);

        return checkClause(orClause, response, OrMode);
    }

    bool found = clause.get(_T("prop"), prop);
    if (!found) {
        throw std::runtime_error("Clauses must contain \"and\", \"or\", or a comparison to make");
    }

    found = clause.get(_T("op"), op);
    if (!found) {
        throw std::runtime_error("Clauses must specify a comparison operation to perform");
    }

    found = clause.get(_T("val"), val);
    if (!found) {
        throw std::runtime_error("Clauses must specify a value to compare against");
    }

    result = checkProperty(prop, response, op, val, mode);

    LOG_AM_DEBUG("Where Trigger: Clause %s %s",
                 MojoObjectJson(clause).c_str(),
                 (result == Matched) ? "matched" : "did not match");

    return result;
}

WhereMatcher::MatchResult WhereMatcher::checkProperty(const MojObject& keyArray,
                                                            MojObject::ConstArrayIterator keyIter,
                                                            const MojObject& responseArray,
                                                            MojObject::ConstArrayIterator responseIter,
                                                            const MojObject& op,
                                                            const MojObject& val,
                                                            MatchMode mode) const
{
    /* Yes, this will iterate into arrays of arrays of arrays */
    for ( ; responseIter != responseArray.arrayEnd() ; ++responseIter) {
        MatchResult result = checkProperty(keyArray, keyIter, *responseIter, op, val, mode);

        if (mode == AndMode) {
            if (result != Matched) {
                return NotMatched;
            }
        } else {
            if (result == Matched) {
                return Matched;
            }
        }
    }

    if (mode == AndMode) {
        return Matched;
    } else {
        return NotMatched;
    }
}

WhereMatcher::MatchResult WhereMatcher::checkProperty(const MojObject& keyArray,
                                                            MojObject::ConstArrayIterator keyIter,
                                                            const MojObject& response,
                                                            const MojObject& op,
                                                            const MojObject& val,
                                                            MatchMode mode) const
{
    MojObject onion = response;
    MojString keyStr;
    MojObject next;
    MojErr err;

    for ( ; keyIter != keyArray.arrayEnd() ; ++keyIter) {
        if (onion.type() == MojObject::TypeArray) {
            return checkProperty(keyArray, keyIter, onion, onion.arrayBegin(), op, val, mode);
        } else if (onion.type() == MojObject::TypeObject) {
            err = (*keyIter).stringValue(keyStr);
            if (err) {
                throw std::runtime_error("Failed to convert property lookup key to string");
            }

            if (!onion.get(keyStr.data(), next)) {
                return NoProperty;
            }

            onion = next;
        } else {
            return NoProperty;
        }
    }

    return checkMatch(onion, op, val);
}

WhereMatcher::MatchResult WhereMatcher::checkProperty(const MojObject& key,
                                                            const MojObject& response,
                                                            const MojObject& op,
                                                            const MojObject& val,
                                                            MatchMode mode) const
{
    MojString keyStr;
    MojErr err;
    MojObject propVal;
    bool found;

    if (key.type() == MojObject::TypeString) {
        err = key.stringValue(keyStr);
        if (err) {
            throw std::runtime_error("Failed to convert property lookup key to string");
        }

        found = response.get(keyStr.data(), propVal);
        if (!found) {
            return NoProperty;
        }

        return checkMatch(propVal, op, val);

    } else if (key.type() == MojObject::TypeArray) {
        return checkProperty(key, key.arrayBegin(), response, op, val, mode);
    } else {
        throw std::runtime_error("Key specified was neither a string or array of strings");
    }
}

WhereMatcher::MatchResult WhereMatcher::checkMatches(const MojObject& rhsArray,
                                                           const MojObject& op,
                                                           const MojObject& val,
                                                           MatchMode mode) const
{
    /* Matching a value against an array */
    for (MojObject::ConstArrayIterator iter = rhsArray.arrayBegin();
        iter != rhsArray.arrayEnd() ; ++iter) {
        MatchResult result = checkMatch(*iter, op, val);
        if (mode == AndMode) {
            if (result != Matched) {
                return NotMatched;
            }
        } else {
            if (result == Matched) {
                return Matched;
            }
        }
    }

    /* If we got here in And mode it means all the values matched.  If we
     * got here in Or mode, it means none of them did. */
    if (mode == AndMode) {
        return Matched;
    } else {
        return NotMatched;
    }
}

WhereMatcher::MatchResult WhereMatcher::checkMatch(const MojObject& rhs,
                                                         const MojObject& op,
                                                         const MojObject& val) const
{
    MojString opStr;
    MojErr err = op.stringValue(opStr);
    if (err) {
        throw std::runtime_error("Failed to convert operation to string value");
    }

    bool result;

    if (opStr == "<") {
        result = (rhs < val);
    } else if (opStr == "<=") {
        result = (rhs <= val);
    } else if (opStr == "=") {
        result = (rhs == val);
    } else if (opStr == "!=") {
        result = (rhs != val);
    } else if (opStr == ">=") {
        result = (rhs >= val);
    } else if (opStr == ">") {
        result = (rhs > val);
    } else if (opStr == "where") {
        result = checkClause(val, rhs, AndMode);
    } else {
        throw std::runtime_error("Unknown comparison operator in where clause");
    }

    if (result) {
        return Matched;
    } else {
        return NotMatched;
    }
}

