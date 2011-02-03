   /*******************************************************/
   /*      "C" Language Integrated Production System      */
   /*                                                     */
   /*             CLIPS Version 6.30  03/05/08            */
   /*                                                     */
   /*                   UTILITY MODULE                    */
   /*******************************************************/

/*************************************************************/
/* Purpose: Provides a set of utility functions useful to    */
/*   other modules. Primarily these are the functions for    */
/*   handling periodic garbage collection and appending      */
/*   string data.                                            */
/*                                                           */
/* Principal Programmer(s):                                  */
/*      Gary D. Riley                                        */
/*                                                           */
/* Contributing Programmer(s):                               */
/*      Brian Dantes                                         */
/*      Jeff Bezanson                                        */
/*         www.cprogramming.com/tutorial/unicode.html        */
/*                                                           */
/* Revision History:                                         */
/*                                                           */
/*      6.24: Renamed BOOLEAN macro type to intBool.         */
/*                                                           */
/*      6.30: Added UTF-8 routines.                          */
/*                                                           */
/*************************************************************/

#define _UTILITY_SOURCE_

#include "setup.h"

#include <ctype.h>
#include <stdlib.h>

#include <stdio.h>
#define _STDIO_INCLUDED_
#include <string.h>

#include "envrnmnt.h"
#include "evaluatn.h"
#include "facthsh.h"
#include "memalloc.h"
#include "multifld.h"
#include "prntutil.h"
#include "sysdep.h"

#include "utility.h"

#define MAX_EPHEMERAL_COUNT 1000L
#define MAX_EPHEMERAL_SIZE 10240L
#define COUNT_INCREMENT 1000L
#define SIZE_INCREMENT 10240L

/***************************************/
/* LOCAL INTERNAL FUNCTION DEFINITIONS */
/***************************************/

   static void                    DeallocateUtilityData(void *,EXEC_STATUS);

/************************************************/
/* InitializeUtilityData: Allocates environment */
/*    data for utility routines.                */
/************************************************/
globle void InitializeUtilityData(
  void *theEnv,
  EXEC_STATUS)
  {
   AllocateEnvironmentData(theEnv,execStatus,UTILITY_DATA,sizeof(struct utilityData),DeallocateUtilityData);
   
   UtilityData(theEnv,execStatus)->GarbageCollectionLocks = 0;
   UtilityData(theEnv,execStatus)->GarbageCollectionHeuristicsEnabled = TRUE;
   UtilityData(theEnv,execStatus)->PeriodicFunctionsEnabled = TRUE;
   UtilityData(theEnv,execStatus)->YieldFunctionEnabled = TRUE;

   UtilityData(theEnv,execStatus)->CurrentEphemeralCountMax = MAX_EPHEMERAL_COUNT;
   UtilityData(theEnv,execStatus)->CurrentEphemeralSizeMax = MAX_EPHEMERAL_SIZE;
   UtilityData(theEnv,execStatus)->LastEvaluationDepth = -1;
  }
  
/**************************************************/
/* DeallocateUtilityData: Deallocates environment */
/*    data for utility routines.                  */
/**************************************************/
static void DeallocateUtilityData(
  void *theEnv,
  EXEC_STATUS)
  {
   struct callFunctionItem *tmpPtr, *nextPtr;
   struct trackedMemory *tmpTM, *nextTM;

   tmpTM = UtilityData(theEnv,execStatus)->trackList;
   while (tmpTM != NULL)
     {
      nextTM = tmpTM->next;
      genfree(theEnv,execStatus,tmpTM->theMemory,tmpTM->memSize);
      rtn_struct(theEnv,execStatus,trackedMemory,tmpTM);
      tmpTM = nextTM;
     }
   
   tmpPtr = UtilityData(theEnv,execStatus)->ListOfPeriodicFunctions;
   while (tmpPtr != NULL)
     {
      nextPtr = tmpPtr->next;
      rtn_struct(theEnv,execStatus,callFunctionItem,tmpPtr);
      tmpPtr = nextPtr;
     }

   tmpPtr = UtilityData(theEnv,execStatus)->ListOfCleanupFunctions;
   while (tmpPtr != NULL)
     {
      nextPtr = tmpPtr->next;
      rtn_struct(theEnv,execStatus,callFunctionItem,tmpPtr);
      tmpPtr = nextPtr;
     }
  }

/*************************************************************/
/* PeriodicCleanup: Returns garbage created during execution */
/*   that has not been returned to the memory pool yet. The  */
/*   cleanup is normally deferred so that an executing rule  */
/*   can still access these data structures. Always calls a  */
/*   series of functions that should be called periodically. */
/*   Usually used by interfaces to update displays.          */
/*************************************************************/
globle void PeriodicCleanup(
  void *theEnv,
  EXEC_STATUS,
  intBool cleanupAllDepths,
  intBool useHeuristics)
  {
   int oldDepth = -1;
   struct callFunctionItem *cleanupPtr,*periodPtr;

   /*===================================*/
   /* Don't use heuristics if disabled. */
   /*===================================*/
   
   if (! UtilityData(theEnv,execStatus)->GarbageCollectionHeuristicsEnabled) 
     { useHeuristics = FALSE; }
     
   /*=============================================*/
   /* Call functions for handling periodic tasks. */
   /*=============================================*/

   if (UtilityData(theEnv,execStatus)->PeriodicFunctionsEnabled)
     {
      for (periodPtr = UtilityData(theEnv,execStatus)->ListOfPeriodicFunctions;
           periodPtr != NULL;
           periodPtr = periodPtr->next)
        { 
         if (periodPtr->environmentAware)
           { (*periodPtr->func)(theEnv,execStatus); }
         else            
           { (* (void (*)(void)) periodPtr->func)(); }
        }
     }
     
   /*===================================================*/
   /* If the last level we performed cleanup was deeper */
   /* than the current level, reset the values used by  */
   /* the heuristics to determine if garbage collection */
   /* should be performed. If the heuristic values had  */
   /* to be incremented because there was no garbage    */
   /* that could be cleaned up, we don't want to keep   */
   /* those same high values permanently so we reset    */
   /* them when we go back to a lower evaluation depth. */
   /*===================================================*/

   if (UtilityData(theEnv,execStatus)->LastEvaluationDepth > execStatus->CurrentEvaluationDepth)
     {
      UtilityData(theEnv,execStatus)->LastEvaluationDepth = execStatus->CurrentEvaluationDepth;
      UtilityData(theEnv,execStatus)->CurrentEphemeralCountMax = MAX_EPHEMERAL_COUNT;
      UtilityData(theEnv,execStatus)->CurrentEphemeralSizeMax = MAX_EPHEMERAL_SIZE;
     }

   /*======================================================*/
   /* If we're using heuristics to determine if garbage    */
   /* collection to occur, then check to see if enough     */
   /* garbage has been created to make cleanup worthwhile. */
   /*======================================================*/

   if (UtilityData(theEnv,execStatus)->GarbageCollectionLocks > 0)  return;
   
   if (useHeuristics &&
       (UtilityData(theEnv,execStatus)->EphemeralItemCount < UtilityData(theEnv,execStatus)->CurrentEphemeralCountMax) &&
       (UtilityData(theEnv,execStatus)->EphemeralItemSize < UtilityData(theEnv,execStatus)->CurrentEphemeralSizeMax))
     { return; }

   /*==========================================================*/
   /* If cleanup is being performed at all depths, rather than */
   /* just the current evaluation depth, then temporarily set  */
   /* the evaluation depth to a level that will force cleanup  */
   /* at all depths.                                           */
   /*==========================================================*/

   if (cleanupAllDepths)
     {
      oldDepth = execStatus->CurrentEvaluationDepth;
      execStatus->CurrentEvaluationDepth = -1;
     }

   /*=============================================*/
   /* Free up multifield values no longer in use. */
   /*=============================================*/

   FlushMultifields(theEnv,execStatus);

   /*=====================================*/
   /* Call the list of cleanup functions. */
   /*=====================================*/

   for (cleanupPtr = UtilityData(theEnv,execStatus)->ListOfCleanupFunctions;
        cleanupPtr != NULL;
        cleanupPtr = cleanupPtr->next)
     {
      if (cleanupPtr->environmentAware)
        { (*cleanupPtr->func)(theEnv,execStatus); }
      else            
        { (* (void (*)(void)) cleanupPtr->func)(); }
    }

   /*================================================*/
   /* Free up atomic values that are no longer used. */
   /*================================================*/

   RemoveEphemeralAtoms(theEnv,execStatus);

   /*=========================================*/
   /* Restore the evaluation depth if cleanup */
   /* was performed on all depths.            */
   /*=========================================*/

   if (cleanupAllDepths) execStatus->CurrentEvaluationDepth = oldDepth;

   /*============================================================*/
   /* If very little memory was freed up, then increment the     */
   /* values used by the heuristics so that we don't continually */
   /* try to free up memory that isn't being released.           */
   /*============================================================*/

   if ((UtilityData(theEnv,execStatus)->EphemeralItemCount + COUNT_INCREMENT) > UtilityData(theEnv,execStatus)->CurrentEphemeralCountMax)
     { UtilityData(theEnv,execStatus)->CurrentEphemeralCountMax = UtilityData(theEnv,execStatus)->EphemeralItemCount + COUNT_INCREMENT; }

   if ((UtilityData(theEnv,execStatus)->EphemeralItemSize + SIZE_INCREMENT) > UtilityData(theEnv,execStatus)->CurrentEphemeralSizeMax)
     { UtilityData(theEnv,execStatus)->CurrentEphemeralSizeMax = UtilityData(theEnv,execStatus)->EphemeralItemSize + SIZE_INCREMENT; }

   /*===============================================================*/
   /* Remember the evaluation depth at which garbage collection was */
   /* last performed. This information is used for resetting the    */
   /* ephemeral count and size numbers used by the heuristics.      */
   /*===============================================================*/

   UtilityData(theEnv,execStatus)->LastEvaluationDepth = execStatus->CurrentEvaluationDepth;
  }

/***************************************************/
/* AddCleanupFunction: Adds a function to the list */
/*   of functions called to perform cleanup such   */
/*   as returning free memory to the memory pool.  */
/***************************************************/
globle intBool AddCleanupFunction(
  void *theEnv,
  EXEC_STATUS,
  char *name,
  void (*theFunction)(void *,EXEC_STATUS),
  int priority)
  {
   UtilityData(theEnv,execStatus)->ListOfCleanupFunctions =
     AddFunctionToCallList(theEnv,execStatus,name,priority,
                           (void (*)(void *,EXEC_STATUS)) theFunction,
                           UtilityData(theEnv,execStatus)->ListOfCleanupFunctions,TRUE);
   return(1);
  }

#if ALLOW_ENVIRONMENT_GLOBALS
/****************************************************/
/* AddPeriodicFunction: Adds a function to the list */
/*   of functions called to handle periodic tasks.  */
/****************************************************/
globle intBool AddPeriodicFunction(
  char *name,
  void (*theFunction)(void),
  int priority)
  {
   void *theEnv = GetCurrentEnvironment();
   EXEC_STATUS  = GetCurrentExecutionStatus();
   
   UtilityData(theEnv,execStatus)->ListOfPeriodicFunctions =
     AddFunctionToCallList(theEnv,execStatus,name,priority,
                           (void (*)(void *,EXEC_STATUS)) theFunction,
                           UtilityData(theEnv,execStatus)->ListOfPeriodicFunctions,FALSE);

   return(1);
  }
#endif

/*******************************************************/
/* EnvAddPeriodicFunction: Adds a function to the list */
/*   of functions called to handle periodic tasks.     */
/*******************************************************/
globle intBool EnvAddPeriodicFunction(
  void *theEnv,
  EXEC_STATUS,
  char *name,
  void (*theFunction)(void *,EXEC_STATUS),
  int priority)
  {
   UtilityData(theEnv,execStatus)->ListOfPeriodicFunctions =
     AddFunctionToCallList(theEnv,execStatus,name,priority,
                           (void (*)(void *,EXEC_STATUS)) theFunction,
                           UtilityData(theEnv,execStatus)->ListOfPeriodicFunctions,TRUE);
   return(1);
  }

/*******************************************************/
/* RemoveCleanupFunction: Removes a function from the  */
/*   list of functions called to perform cleanup such  */
/*   as returning free memory to the memory pool.      */
/*******************************************************/
globle intBool RemoveCleanupFunction(
  void *theEnv,
  EXEC_STATUS,
  char *name)
  {
   intBool found;
   
   UtilityData(theEnv,execStatus)->ListOfCleanupFunctions =
      RemoveFunctionFromCallList(theEnv,execStatus,name,UtilityData(theEnv,execStatus)->ListOfCleanupFunctions,&found);
  
   return found;
  }

/**********************************************************/
/* EnvRemovePeriodicFunction: Removes a function from the */
/*   list of functions called to handle periodic tasks.   */
/**********************************************************/
globle intBool EnvRemovePeriodicFunction(
  void *theEnv,
  EXEC_STATUS,
  char *name)
  {
   intBool found;
   
   UtilityData(theEnv,execStatus)->ListOfPeriodicFunctions =
      RemoveFunctionFromCallList(theEnv,execStatus,name,UtilityData(theEnv,execStatus)->ListOfPeriodicFunctions,&found);
  
   return found;
  }

/*****************************************************/
/* StringPrintForm: Generates printed representation */
/*   of a string. Replaces / with // and " with /".  */
/*****************************************************/
globle char *StringPrintForm(
  void *theEnv,
  EXEC_STATUS,
  char *str)
  {
   int i = 0;
   size_t pos = 0;
   size_t max = 0;
   char *theString = NULL;
   void *thePtr;

   theString = ExpandStringWithChar(theEnv,execStatus,'"',theString,&pos,&max,max+80);
   while (str[i] != EOS)
     {
      if ((str[i] == '"') || (str[i] == '\\'))
        {
         theString = ExpandStringWithChar(theEnv,execStatus,'\\',theString,&pos,&max,max+80);
         theString = ExpandStringWithChar(theEnv,execStatus,str[i],theString,&pos,&max,max+80);
        }
      else
        { theString = ExpandStringWithChar(theEnv,execStatus,str[i],theString,&pos,&max,max+80); }
      i++;
     }

   theString = ExpandStringWithChar(theEnv,execStatus,'"',theString,&pos,&max,max+80);

   thePtr = EnvAddSymbol(theEnv,execStatus,theString);
   rm(theEnv,execStatus,theString,max);
   return(ValueToString(thePtr));
  }

/***********************************************************/
/* AppendStrings: Appends two strings together. The string */
/*   created is added to the SymbolTable, so it is not     */
/*   necessary to deallocate the string returned.          */
/***********************************************************/
globle char *AppendStrings(
  void *theEnv,
  EXEC_STATUS,
  char *str1,
  char *str2)
  {
   size_t pos = 0;
   size_t max = 0;
   char *theString = NULL;
   void *thePtr;

   theString = AppendToString(theEnv,execStatus,str1,theString,&pos,&max);
   theString = AppendToString(theEnv,execStatus,str2,theString,&pos,&max);

   thePtr = EnvAddSymbol(theEnv,execStatus,theString);
   rm(theEnv,execStatus,theString,max);
   return(ValueToString(thePtr));
  }

/******************************************************/
/* AppendToString: Appends a string to another string */
/*   (expanding the other string if necessary).       */
/******************************************************/
globle char *AppendToString(
  void *theEnv,
  EXEC_STATUS,
  char *appendStr,
  char *oldStr,
  size_t *oldPos,
  size_t *oldMax)
  {
   size_t length;

   /*=========================================*/
   /* Expand the old string so it can contain */
   /* the new string (if necessary).          */
   /*=========================================*/

   length = strlen(appendStr);

   /*==============================================================*/
   /* Return NULL if the old string was not successfully expanded. */
   /*==============================================================*/

   if ((oldStr = EnlargeString(theEnv,execStatus,length,oldStr,oldPos,oldMax)) == NULL) { return(NULL); }

   /*===============================================*/
   /* Append the new string to the expanded string. */
   /*===============================================*/

   genstrcpy(&oldStr[*oldPos],appendStr);
   *oldPos += (int) length;

   /*============================================================*/
   /* Return the expanded string containing the appended string. */
   /*============================================================*/

   return(oldStr);
  }

/**********************************************************/
/* InsertInString: Inserts a string within another string */
/*   (expanding the other string if necessary).           */
/**********************************************************/
globle char *InsertInString(
  void *theEnv,
  EXEC_STATUS,
  char *insertStr,
  size_t position,
  char *oldStr,
  size_t *oldPos,
  size_t *oldMax)
  {
   size_t length;

   /*=========================================*/
   /* Expand the old string so it can contain */
   /* the new string (if necessary).          */
   /*=========================================*/

   length = strlen(insertStr);

   /*==============================================================*/
   /* Return NULL if the old string was not successfully expanded. */
   /*==============================================================*/

   if ((oldStr = EnlargeString(theEnv,execStatus,length,oldStr,oldPos,oldMax)) == NULL) { return(NULL); }

   /*================================================================*/
   /* Shift the contents to the right of insertion point so that the */
   /* new text does not overwrite what is currently in the string.   */
   /*================================================================*/
   
   memmove(&oldStr[position],&oldStr[position+length],*oldPos - position);

   /*===============================================*/
   /* Insert the new string in the expanded string. */
   /*===============================================*/

   genstrncpy(&oldStr[*oldPos],insertStr,length);
   *oldPos += (int) length;

   /*============================================================*/
   /* Return the expanded string containing the appended string. */
   /*============================================================*/

   return(oldStr);
  }
  
/*******************************************************************/
/* EnlargeString: Enlarges a string by the specified amount.       */
/*******************************************************************/
globle char *EnlargeString(
  void *theEnv,
  EXEC_STATUS,
  size_t length,
  char *oldStr,
  size_t *oldPos,
  size_t *oldMax)
  {
   /*=========================================*/
   /* Expand the old string so it can contain */
   /* the new string (if necessary).          */
   /*=========================================*/

   if (length + *oldPos + 1 > *oldMax)
     {
      oldStr = (char *) genrealloc(theEnv,execStatus,oldStr,*oldMax,length + *oldPos + 1);
      *oldMax = length + *oldPos + 1;
     }

   /*==============================================================*/
   /* Return NULL if the old string was not successfully expanded. */
   /*==============================================================*/

   if (oldStr == NULL) { return(NULL); }

   return(oldStr);
  }

/*******************************************************/
/* AppendNToString: Appends a string to another string */
/*   (expanding the other string if necessary). Only a */
/*   specified number of characters are appended from  */
/*   the string.                                       */
/*******************************************************/
globle char *AppendNToString(
  void *theEnv,
  EXEC_STATUS,
  char *appendStr,
  char *oldStr,
  size_t length,
  size_t *oldPos,
  size_t *oldMax)
  {
   size_t lengthWithEOS;

   /*====================================*/
   /* Determine the number of characters */
   /* to be appended from the string.    */
   /*====================================*/

   if (appendStr[length-1] != '\0') lengthWithEOS = length + 1;
   else lengthWithEOS = length;

   /*=========================================*/
   /* Expand the old string so it can contain */
   /* the new string (if necessary).          */
   /*=========================================*/

   if (lengthWithEOS + *oldPos > *oldMax)
     {
      oldStr = (char *) genrealloc(theEnv,execStatus,oldStr,*oldMax,*oldPos + lengthWithEOS);
      *oldMax = *oldPos + lengthWithEOS;
     }

   /*==============================================================*/
   /* Return NULL if the old string was not successfully expanded. */
   /*==============================================================*/

   if (oldStr == NULL) { return(NULL); }

   /*==================================*/
   /* Append N characters from the new */
   /* string to the expanded string.   */
   /*==================================*/

   genstrncpy(&oldStr[*oldPos],appendStr,length);
   *oldPos += (lengthWithEOS - 1);
   oldStr[*oldPos] = '\0';

   /*============================================================*/
   /* Return the expanded string containing the appended string. */
   /*============================================================*/

   return(oldStr);
  }

/*******************************************************/
/* ExpandStringWithChar: Adds a character to a string, */
/*   reallocating space for the string if it needs to  */
/*   be enlarged. The backspace character causes the   */
/*   size of the string to reduced if it is "added" to */
/*   the string.                                       */
/*******************************************************/
globle char *ExpandStringWithChar(
  void *theEnv,
  EXEC_STATUS,
  int inchar,
  char *str,
  size_t *pos,
  size_t *max,
  size_t newSize)
  {
   if ((*pos + 1) >= *max)
     {
      str = (char *) genrealloc(theEnv,execStatus,str,*max,newSize);
      *max = newSize;
     }

  if (inchar != '\b')
    {
     str[*pos] = (char) inchar;
     (*pos)++;
     str[*pos] = '\0';
    }
  else
    {
     /*===========================================================*/
     /* First delete any UTF-8 multibyte continuation characters. */
     /*===========================================================*/

     while ((*pos > 1) && IsUTF8MultiByteContinuation(str[*pos - 1]))
       { (*pos)--; }
     
     /*===================================================*/
     /* Now delete the first byte of the UTF-8 character. */
     /*===================================================*/
     
     if (*pos > 0) (*pos)--;
     str[*pos] = '\0';
    }

   return(str);
  }

/*****************************************************************/
/* AddFunctionToCallList: Adds a function to a list of functions */
/*   which are called to perform certain operations (e.g. clear, */
/*   reset, and bload functions).                                */
/*****************************************************************/
globle struct callFunctionItem *AddFunctionToCallList(
  void *theEnv,
  EXEC_STATUS,
  char *name,
  int priority,
  void (*func)(void *,EXEC_STATUS),
  struct callFunctionItem *head,
  intBool environmentAware)
  {
   return AddFunctionToCallListWithContext(theEnv,execStatus,name,priority,func,head,environmentAware,NULL);
  }
  
/***********************************************************/
/* AddFunctionToCallListWithContext: Adds a function to a  */
/*   list of functions which are called to perform certain */
/*   operations (e.g. clear, reset, and bload functions).  */
/***********************************************************/
globle struct callFunctionItem *AddFunctionToCallListWithContext(
  void *theEnv,
  EXEC_STATUS,
  char *name,
  int priority,
  void (*func)(void *,EXEC_STATUS),
  struct callFunctionItem *head,
  intBool environmentAware,
  void *context)
  {
   struct callFunctionItem *newPtr, *currentPtr, *lastPtr = NULL;

   newPtr = get_struct(theEnv,execStatus,callFunctionItem);

   newPtr->name = name;
   newPtr->func = func;
   newPtr->priority = priority;
   newPtr->environmentAware = (short) environmentAware;
   newPtr->context = context;

   if (head == NULL)
     {
      newPtr->next = NULL;
      return(newPtr);
     }

   currentPtr = head;
   while ((currentPtr != NULL) ? (priority < currentPtr->priority) : FALSE)
     {
      lastPtr = currentPtr;
      currentPtr = currentPtr->next;
     }

   if (lastPtr == NULL)
     {
      newPtr->next = head;
      head = newPtr;
     }
   else
     {
      newPtr->next = currentPtr;
      lastPtr->next = newPtr;
     }

   return(head);
  }

/*****************************************************************/
/* RemoveFunctionFromCallList: Removes a function from a list of */
/*   functions which are called to perform certain operations    */
/*   (e.g. clear, reset, and bload functions).                   */
/*****************************************************************/
globle struct callFunctionItem *RemoveFunctionFromCallList(
  void *theEnv,
  EXEC_STATUS,
  char *name,
  struct callFunctionItem *head,
  int *found)
  {
   struct callFunctionItem *currentPtr, *lastPtr;

   *found = FALSE;
   lastPtr = NULL;
   currentPtr = head;

   while (currentPtr != NULL)
     {
      if (strcmp(name,currentPtr->name) == 0)
        {
         *found = TRUE;
         if (lastPtr == NULL)
           { head = currentPtr->next; }
         else
           { lastPtr->next = currentPtr->next; }

         rtn_struct(theEnv,execStatus,callFunctionItem,currentPtr);
         return(head);
        }

      lastPtr = currentPtr;
      currentPtr = currentPtr->next;
     }

   return(head);
  }

/**************************************************************/
/* DeallocateCallList: Removes all functions from a list of   */
/*   functions which are called to perform certain operations */
/*   (e.g. clear, reset, and bload functions).                */
/**************************************************************/
globle void DeallocateCallList(
  void *theEnv,
  EXEC_STATUS,
  struct callFunctionItem *theList)
  {
   struct callFunctionItem *tmpPtr, *nextPtr;
   
   tmpPtr = theList;
   while (tmpPtr != NULL)
     {
      nextPtr = tmpPtr->next;
      rtn_struct(theEnv,execStatus,callFunctionItem,tmpPtr);
      tmpPtr = nextPtr;
     }
  }

/*****************************************/
/* ItemHashValue: Returns the hash value */
/*   for the specified value.            */
/*****************************************/
globle unsigned long ItemHashValue(
  void *theEnv,
  EXEC_STATUS,
  unsigned short theType,
  void *theValue,
  unsigned long theRange)
  {
   union
     {
      void *vv;
      unsigned uv;
     } fis;
     
   switch(theType)
     {
      case FLOAT:
        return(HashFloat(ValueToDouble(theValue),theRange));

      case INTEGER:
        return(HashInteger(ValueToLong(theValue),theRange));

      case SYMBOL:
      case STRING:
#if OBJECT_SYSTEM
      case INSTANCE_NAME:
#endif
        return(HashSymbol(ValueToString(theValue),theRange));

      case MULTIFIELD:
        return(HashMultifield((struct multifield *) theValue,theRange));

#if DEFTEMPLATE_CONSTRUCT
      case FACT_ADDRESS:
        return(((struct fact *) theValue)->hashValue % theRange);
#endif

      case EXTERNAL_ADDRESS:
        return(HashExternalAddress(ValueToExternalAddress(theValue),theRange));
        
#if OBJECT_SYSTEM
      case INSTANCE_ADDRESS:
#endif
        fis.uv = 0;
        fis.vv = theValue;
        return(fis.uv % theRange);
     }

   SystemError(theEnv,execStatus,"UTILITY",1);
   return(0);
  }

/********************************************/
/* YieldTime: Yields time to a user-defined */
/*   function. Intended to allow foreground */
/*   application responsiveness when CLIPS  */
/*   is running in the background.          */
/********************************************/
globle void YieldTime(
  void *theEnv,
  EXEC_STATUS)
  {
   if ((UtilityData(theEnv,execStatus)->YieldTimeFunction != NULL) && UtilityData(theEnv,execStatus)->YieldFunctionEnabled)
     { (*UtilityData(theEnv,execStatus)->YieldTimeFunction)(); }
  }
  
/********************************************/
/* SetGarbageCollectionHeuristics:         */
/********************************************/
globle short SetGarbageCollectionHeuristics(
  void *theEnv,
  EXEC_STATUS,
  short newValue)
  {
   short oldValue;

   oldValue = UtilityData(theEnv,execStatus)->GarbageCollectionHeuristicsEnabled;
   
   UtilityData(theEnv,execStatus)->GarbageCollectionHeuristicsEnabled = newValue;
   
   return(oldValue);
  }
 
/**********************************************/
/* EnvIncrementGCLocks: Increments the number */
/*   of garbage collection locks.             */
/**********************************************/
globle void EnvIncrementGCLocks(
  void *theEnv,
  EXEC_STATUS)
  {
   UtilityData(theEnv,execStatus)->GarbageCollectionLocks++;
  }

/**********************************************/
/* EnvDecrementGCLocks: Decrements the number */
/*   of garbage collection locks.             */
/**********************************************/
globle void EnvDecrementGCLocks(
  void *theEnv,
  EXEC_STATUS)
  {
   if (UtilityData(theEnv,execStatus)->GarbageCollectionLocks > 0)
     { UtilityData(theEnv,execStatus)->GarbageCollectionLocks--; }
  }
 
/********************************************/
/* EnablePeriodicFunctions:         */
/********************************************/
globle short EnablePeriodicFunctions(
  void *theEnv,
  EXEC_STATUS,
  short value)
  {
   short oldValue;
   
   oldValue = UtilityData(theEnv,execStatus)->PeriodicFunctionsEnabled;
   
   UtilityData(theEnv,execStatus)->PeriodicFunctionsEnabled = value;
   
   return(oldValue);
  }
  
/********************************************/
/* EnableYieldFunction:         */
/********************************************/
globle short EnableYieldFunction(
  void *theEnv,
  EXEC_STATUS,
  short value)
  {
   short oldValue;
   
   oldValue = UtilityData(theEnv,execStatus)->YieldFunctionEnabled;
   
   UtilityData(theEnv,execStatus)->YieldFunctionEnabled = value;
   
   return(oldValue);
  }

/********************************************/
/* AddTrackedMemory: */
/********************************************/
globle struct trackedMemory *AddTrackedMemory(
  void *theEnv,
  EXEC_STATUS,
  void *theMemory,
  size_t theSize)
  {
   struct trackedMemory *newPtr;
   
   newPtr = get_struct(theEnv,execStatus,trackedMemory);
   
   newPtr->prev = NULL;
   newPtr->theMemory = theMemory;
   newPtr->memSize = theSize;
   newPtr->next = UtilityData(theEnv,execStatus)->trackList;
   UtilityData(theEnv,execStatus)->trackList = newPtr;
   
   return newPtr;
  }

/********************************************/
/* RemoveTrackedMemory: */
/********************************************/
globle void RemoveTrackedMemory(
  void *theEnv,
  EXEC_STATUS,
  struct trackedMemory *theTracker)
  {   
   if (theTracker->prev == NULL)
     { UtilityData(theEnv,execStatus)->trackList = theTracker->next; }
   else
     { theTracker->prev->next = theTracker->next; }
     
   if (theTracker->next != NULL)
     { theTracker->next->prev = theTracker->prev; }
     
   rtn_struct(theEnv,execStatus,trackedMemory,theTracker);
  }

/******************************************/
/* UTF8Length: Returns the logical number */
/*   of characters in a UTF8 string.      */
/******************************************/
globle size_t UTF8Length(
  char *s)
  {
   size_t i = 0, length = 0;
   
   while (s[i] != '\0')
     { 
      UTF8Increment(s,&i); 
      length++;
     }
   
   return(length);
  }
  
/*********************************************/
/* UTF8Increment: Finds the beginning of the */
/*   next character in a UTF8 string.        */
/*********************************************/
globle void UTF8Increment(
  char *s,
  size_t *i)
  {
   (void) (IsUTF8Start(s[++(*i)]) || 
           IsUTF8Start(s[++(*i)]) ||
           IsUTF8Start(s[++(*i)]) || 
           ++(*i));
  }

/****************************************************/
/* UTF8Offset: Converts the logical character index */
/*   in a UTF8 string to the actual byte offset.    */
/****************************************************/
globle size_t UTF8Offset(
  char *str, 
  size_t charnum)
  {
   size_t offs = 0;

   while ((charnum > 0) && (str[offs])) 
     {
      (void) (IsUTF8Start(str[++offs]) || 
              IsUTF8Start(str[++offs]) ||
              IsUTF8Start(str[++offs]) || 
              ++offs);
              
      charnum--;
     }
     
   return offs;
  }

/*************************************************/
/* UTF8CharNum: Converts the UTF8 character byte */ 
/*   offset to the logical character index.      */
/*************************************************/
globle size_t UTF8CharNum(
  char *s, 
  size_t offset)
  {
   size_t charnum = 0, offs=0;

   while ((offs < offset) && (s[offs])) 
     {
      (void) (IsUTF8Start(s[++offs]) ||
              IsUTF8Start(s[++offs]) ||
              IsUTF8Start(s[++offs]) || 
              ++offs);
              
      charnum++;
     }
     
   return charnum;
  }

