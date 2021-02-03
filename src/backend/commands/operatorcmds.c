/*-------------------------------------------------------------------------
 *
 * operatorcmds.c
 *
 *	  Routines for operator manipulation commands
 *
 * Portions Copyright (c) 1996-2021, PostgreSQL Global Development Group
 * Portions Copyright (c) 1994, Regents of the University of California
 *
 *
 * IDENTIFICATION
 *	  src/backend/commands/operatorcmds.c
 *
 * DESCRIPTION
 *	  The "DefineFoo" routines take the parse tree and pick out the
 *	  appropriate arguments/flags, passing the results to the
 *	  corresponding "FooDefine" routines (in src/catalog) that do
 *	  the actual catalog-munging.  These routines also verify permission
 *	  of the user to execute the command.
 *
 * NOTES
 *	  These things must be defined and committed in the following order:
 *		"create function":
 *				input/output, recv/send functions
 *		"create type":
 *				type
 *		"create operator":
 *				operators
 *
 *-------------------------------------------------------------------------
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/dependency.h"
#include "catalog/indexing.h"
#include "catalog/objectaccess.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_type.h"
#include "commands/alter.h"
#include "commands/defrem.h"
#include "miscadmin.h"
#include "parser/parse_func.h"
#include "parser/parse_oper.h"
#include "parser/parse_type.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

static Oid	ValidateRestrictionEstimator(List *restrictionName);
static Oid	ValidateJoinEstimator(List *joinName);

/*
 * DefineOperator
 *		this function extracts all the information from the
 *		parameter list generated by the parser and then has
 *		OperatorCreate() do all the actual work.
 *
 * 'parameters' is a list of DefElem
 */
ObjectAddress
DefineOperator(List *names, List *parameters)
{
	char	   *oprName;
	Oid			oprNamespace;
	AclResult	aclresult;
	bool		canMerge = false;	/* operator merges */
	bool		canHash = false;	/* operator hashes */
	List	   *functionName = NIL; /* function for operator */
	TypeName   *typeName1 = NULL;	/* first type name */
	TypeName   *typeName2 = NULL;	/* second type name */
	Oid			typeId1 = InvalidOid;	/* types converted to OID */
	Oid			typeId2 = InvalidOid;
	Oid			rettype;
	List	   *commutatorName = NIL;	/* optional commutator operator name */
	List	   *negatorName = NIL;	/* optional negator operator name */
	List	   *restrictionName = NIL;	/* optional restrict. sel. function */
	List	   *joinName = NIL; /* optional join sel. function */
	Oid			functionOid;	/* functions converted to OID */
	Oid			restrictionOid;
	Oid			joinOid;
	Oid			typeId[2];		/* to hold left and right arg */
	int			nargs;
	ListCell   *pl;

	/* Convert list of names to a name and namespace */
	oprNamespace = QualifiedNameGetCreationNamespace(names, &oprName);

	/* Check we have creation rights in target namespace */
	aclresult = pg_namespace_aclcheck(oprNamespace, GetUserId(), ACL_CREATE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_SCHEMA,
					   get_namespace_name(oprNamespace));

	/*
	 * loop over the definition list and extract the information we need.
	 */
	foreach(pl, parameters)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);

		if (strcmp(defel->defname, "leftarg") == 0)
		{
			typeName1 = defGetTypeName(defel);
			if (typeName1->setof)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("SETOF type not allowed for operator argument")));
		}
		else if (strcmp(defel->defname, "rightarg") == 0)
		{
			typeName2 = defGetTypeName(defel);
			if (typeName2->setof)
				ereport(ERROR,
						(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
						 errmsg("SETOF type not allowed for operator argument")));
		}
		/* "function" and "procedure" are equivalent here */
		else if (strcmp(defel->defname, "function") == 0)
			functionName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "procedure") == 0)
			functionName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "commutator") == 0)
			commutatorName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "negator") == 0)
			negatorName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "restrict") == 0)
			restrictionName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "join") == 0)
			joinName = defGetQualifiedName(defel);
		else if (strcmp(defel->defname, "hashes") == 0)
			canHash = defGetBoolean(defel);
		else if (strcmp(defel->defname, "merges") == 0)
			canMerge = defGetBoolean(defel);
		/* These obsolete options are taken as meaning canMerge */
		else if (strcmp(defel->defname, "sort1") == 0)
			canMerge = true;
		else if (strcmp(defel->defname, "sort2") == 0)
			canMerge = true;
		else if (strcmp(defel->defname, "ltcmp") == 0)
			canMerge = true;
		else if (strcmp(defel->defname, "gtcmp") == 0)
			canMerge = true;
		else
		{
			/* WARNING, not ERROR, for historical backwards-compatibility */
			ereport(WARNING,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("operator attribute \"%s\" not recognized",
							defel->defname)));
		}
	}

	/*
	 * make sure we have our required definitions
	 */
	if (functionName == NIL)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("operator function must be specified")));

	/* Transform type names to type OIDs */
	if (typeName1)
		typeId1 = typenameTypeId(NULL, typeName1);
	if (typeName2)
		typeId2 = typenameTypeId(NULL, typeName2);

	/*
	 * If only the right argument is missing, the user is likely trying to
	 * create a postfix operator, so give them a hint about why that does not
	 * work.  But if both arguments are missing, do not mention postfix
	 * operators, as the user most likely simply neglected to mention the
	 * arguments.
	 */
	if (!OidIsValid(typeId1) && !OidIsValid(typeId2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("operator argument types must be specified")));
	if (!OidIsValid(typeId2))
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
				 errmsg("operator right argument type must be specified"),
				 errdetail("Postfix operators are not supported.")));

	if (typeName1)
	{
		aclresult = pg_type_aclcheck(typeId1, GetUserId(), ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error_type(aclresult, typeId1);
	}

	if (typeName2)
	{
		aclresult = pg_type_aclcheck(typeId2, GetUserId(), ACL_USAGE);
		if (aclresult != ACLCHECK_OK)
			aclcheck_error_type(aclresult, typeId2);
	}

	/*
	 * Look up the operator's underlying function.
	 */
	if (!OidIsValid(typeId1))
	{
		typeId[0] = typeId2;
		nargs = 1;
	}
	else if (!OidIsValid(typeId2))
	{
		typeId[0] = typeId1;
		nargs = 1;
	}
	else
	{
		typeId[0] = typeId1;
		typeId[1] = typeId2;
		nargs = 2;
	}
	functionOid = LookupFuncName(functionName, nargs, typeId, false);

	/*
	 * We require EXECUTE rights for the function.  This isn't strictly
	 * necessary, since EXECUTE will be checked at any attempted use of the
	 * operator, but it seems like a good idea anyway.
	 */
	aclresult = pg_proc_aclcheck(functionOid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION,
					   NameListToString(functionName));

	rettype = get_func_rettype(functionOid);
	aclresult = pg_type_aclcheck(rettype, GetUserId(), ACL_USAGE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error_type(aclresult, rettype);

	/*
	 * Look up restriction and join estimators if specified
	 */
	if (restrictionName)
		restrictionOid = ValidateRestrictionEstimator(restrictionName);
	else
		restrictionOid = InvalidOid;
	if (joinName)
		joinOid = ValidateJoinEstimator(joinName);
	else
		joinOid = InvalidOid;

	/*
	 * now have OperatorCreate do all the work..
	 */
	return
		OperatorCreate(oprName, /* operator name */
					   oprNamespace,	/* namespace */
					   typeId1, /* left type id */
					   typeId2, /* right type id */
					   functionOid, /* function for operator */
					   commutatorName,	/* optional commutator operator name */
					   negatorName, /* optional negator operator name */
					   restrictionOid,	/* optional restrict. sel. function */
					   joinOid, /* optional join sel. function name */
					   canMerge,	/* operator merges */
					   canHash);	/* operator hashes */
}

/*
 * Look up a restriction estimator function ny name, and verify that it has
 * the correct signature and we have the permissions to attach it to an
 * operator.
 */
static Oid
ValidateRestrictionEstimator(List *restrictionName)
{
	Oid			typeId[4];
	Oid			restrictionOid;
	AclResult	aclresult;

	typeId[0] = INTERNALOID;	/* PlannerInfo */
	typeId[1] = OIDOID;			/* operator OID */
	typeId[2] = INTERNALOID;	/* args list */
	typeId[3] = INT4OID;		/* varRelid */

	restrictionOid = LookupFuncName(restrictionName, 4, typeId, false);

	/* estimators must return float8 */
	if (get_func_rettype(restrictionOid) != FLOAT8OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("restriction estimator function %s must return type %s",
						NameListToString(restrictionName), "float8")));

	/* Require EXECUTE rights for the estimator */
	aclresult = pg_proc_aclcheck(restrictionOid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION,
					   NameListToString(restrictionName));

	return restrictionOid;
}

/*
 * Look up a join estimator function ny name, and verify that it has the
 * correct signature and we have the permissions to attach it to an
 * operator.
 */
static Oid
ValidateJoinEstimator(List *joinName)
{
	Oid			typeId[5];
	Oid			joinOid;
	Oid			joinOid2;
	AclResult	aclresult;

	typeId[0] = INTERNALOID;	/* PlannerInfo */
	typeId[1] = OIDOID;			/* operator OID */
	typeId[2] = INTERNALOID;	/* args list */
	typeId[3] = INT2OID;		/* jointype */
	typeId[4] = INTERNALOID;	/* SpecialJoinInfo */

	/*
	 * As of Postgres 8.4, the preferred signature for join estimators has 5
	 * arguments, but we still allow the old 4-argument form.  Whine about
	 * ambiguity if both forms exist.
	 */
	joinOid = LookupFuncName(joinName, 5, typeId, true);
	joinOid2 = LookupFuncName(joinName, 4, typeId, true);
	if (OidIsValid(joinOid))
	{
		if (OidIsValid(joinOid2))
			ereport(ERROR,
					(errcode(ERRCODE_AMBIGUOUS_FUNCTION),
					 errmsg("join estimator function %s has multiple matches",
							NameListToString(joinName))));
	}
	else
	{
		joinOid = joinOid2;
		/* If not found, reference the 5-argument signature in error msg */
		if (!OidIsValid(joinOid))
			joinOid = LookupFuncName(joinName, 5, typeId, false);
	}

	/* estimators must return float8 */
	if (get_func_rettype(joinOid) != FLOAT8OID)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_OBJECT_DEFINITION),
				 errmsg("join estimator function %s must return type %s",
						NameListToString(joinName), "float8")));

	/* Require EXECUTE rights for the estimator */
	aclresult = pg_proc_aclcheck(joinOid, GetUserId(), ACL_EXECUTE);
	if (aclresult != ACLCHECK_OK)
		aclcheck_error(aclresult, OBJECT_FUNCTION,
					   NameListToString(joinName));

	return joinOid;
}

/*
 * Guts of operator deletion.
 */
void
RemoveOperatorById(Oid operOid)
{
	Relation	relation;
	HeapTuple	tup;
	Form_pg_operator op;

	relation = table_open(OperatorRelationId, RowExclusiveLock);

	tup = SearchSysCache1(OPEROID, ObjectIdGetDatum(operOid));
	if (!HeapTupleIsValid(tup)) /* should not happen */
		elog(ERROR, "cache lookup failed for operator %u", operOid);
	op = (Form_pg_operator) GETSTRUCT(tup);

	/*
	 * Reset links from commutator and negator, if any.  In case of a
	 * self-commutator or self-negator, this means we have to re-fetch the
	 * updated tuple.  (We could optimize away updates on the tuple we're
	 * about to drop, but it doesn't seem worth convoluting the logic for.)
	 */
	if (OidIsValid(op->oprcom) || OidIsValid(op->oprnegate))
	{
		OperatorUpd(operOid, op->oprcom, op->oprnegate, true);
		if (operOid == op->oprcom || operOid == op->oprnegate)
		{
			ReleaseSysCache(tup);
			tup = SearchSysCache1(OPEROID, ObjectIdGetDatum(operOid));
			if (!HeapTupleIsValid(tup)) /* should not happen */
				elog(ERROR, "cache lookup failed for operator %u", operOid);
		}
	}

	CatalogTupleDelete(relation, &tup->t_self);

	ReleaseSysCache(tup);

	table_close(relation, RowExclusiveLock);
}

/*
 * AlterOperator
 *		routine implementing ALTER OPERATOR <operator> SET (option = ...).
 *
 * Currently, only RESTRICT and JOIN estimator functions can be changed.
 */
ObjectAddress
AlterOperator(AlterOperatorStmt *stmt)
{
	ObjectAddress address;
	Oid			oprId;
	Relation	catalog;
	HeapTuple	tup;
	Form_pg_operator oprForm;
	int			i;
	ListCell   *pl;
	Datum		values[Natts_pg_operator];
	bool		nulls[Natts_pg_operator];
	bool		replaces[Natts_pg_operator];
	List	   *restrictionName = NIL;	/* optional restrict. sel. function */
	bool		updateRestriction = false;
	Oid			restrictionOid;
	List	   *joinName = NIL; /* optional join sel. function */
	bool		updateJoin = false;
	Oid			joinOid;

	/* Look up the operator */
	oprId = LookupOperWithArgs(stmt->opername, false);
	catalog = table_open(OperatorRelationId, RowExclusiveLock);
	tup = SearchSysCacheCopy1(OPEROID, ObjectIdGetDatum(oprId));
	if (!HeapTupleIsValid(tup))
		elog(ERROR, "cache lookup failed for operator %u", oprId);
	oprForm = (Form_pg_operator) GETSTRUCT(tup);

	/* Process options */
	foreach(pl, stmt->options)
	{
		DefElem    *defel = (DefElem *) lfirst(pl);
		List	   *param;

		if (defel->arg == NULL)
			param = NIL;		/* NONE, removes the function */
		else
			param = defGetQualifiedName(defel);

		if (strcmp(defel->defname, "restrict") == 0)
		{
			restrictionName = param;
			updateRestriction = true;
		}
		else if (strcmp(defel->defname, "join") == 0)
		{
			joinName = param;
			updateJoin = true;
		}

		/*
		 * The rest of the options that CREATE accepts cannot be changed.
		 * Check for them so that we can give a meaningful error message.
		 */
		else if (strcmp(defel->defname, "leftarg") == 0 ||
				 strcmp(defel->defname, "rightarg") == 0 ||
				 strcmp(defel->defname, "function") == 0 ||
				 strcmp(defel->defname, "procedure") == 0 ||
				 strcmp(defel->defname, "commutator") == 0 ||
				 strcmp(defel->defname, "negator") == 0 ||
				 strcmp(defel->defname, "hashes") == 0 ||
				 strcmp(defel->defname, "merges") == 0)
		{
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("operator attribute \"%s\" cannot be changed",
							defel->defname)));
		}
		else
			ereport(ERROR,
					(errcode(ERRCODE_SYNTAX_ERROR),
					 errmsg("operator attribute \"%s\" not recognized",
							defel->defname)));
	}

	/* Check permissions. Must be owner. */
	if (!pg_oper_ownercheck(oprId, GetUserId()))
		aclcheck_error(ACLCHECK_NOT_OWNER, OBJECT_OPERATOR,
					   NameStr(oprForm->oprname));

	/*
	 * Look up restriction and join estimators if specified
	 */
	if (restrictionName)
		restrictionOid = ValidateRestrictionEstimator(restrictionName);
	else
		restrictionOid = InvalidOid;
	if (joinName)
		joinOid = ValidateJoinEstimator(joinName);
	else
		joinOid = InvalidOid;

	/* Perform additional checks, like OperatorCreate does */
	if (!(OidIsValid(oprForm->oprleft) && OidIsValid(oprForm->oprright)))
	{
		/* If it's not a binary op, these things mustn't be set: */
		if (OidIsValid(joinOid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only binary operators can have join selectivity")));
	}

	if (oprForm->oprresult != BOOLOID)
	{
		if (OidIsValid(restrictionOid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only boolean operators can have restriction selectivity")));
		if (OidIsValid(joinOid))
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_FUNCTION_DEFINITION),
					 errmsg("only boolean operators can have join selectivity")));
	}

	/* Update the tuple */
	for (i = 0; i < Natts_pg_operator; ++i)
	{
		values[i] = (Datum) 0;
		replaces[i] = false;
		nulls[i] = false;
	}
	if (updateRestriction)
	{
		replaces[Anum_pg_operator_oprrest - 1] = true;
		values[Anum_pg_operator_oprrest - 1] = restrictionOid;
	}
	if (updateJoin)
	{
		replaces[Anum_pg_operator_oprjoin - 1] = true;
		values[Anum_pg_operator_oprjoin - 1] = joinOid;
	}

	tup = heap_modify_tuple(tup, RelationGetDescr(catalog),
							values, nulls, replaces);

	CatalogTupleUpdate(catalog, &tup->t_self, tup);

	address = makeOperatorDependencies(tup, true);

	InvokeObjectPostAlterHook(OperatorRelationId, oprId, 0);

	table_close(catalog, NoLock);

	return address;
}
