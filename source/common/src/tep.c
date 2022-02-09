#include "tep.h"
#include "common.h"
#include "tglobal.h"
#include "tlockfree.h"

int taosGetFqdnPortFromEp(const char *ep, SEp* pEp) {
  pEp->port = 0;
  strcpy(pEp->fqdn, ep);

  char *temp = strchr(pEp->fqdn, ':');
  if (temp) {
    *temp = 0;
    pEp->port = atoi(temp+1);
  }

  if (pEp->port == 0) {
    pEp->port = tsServerPort;
    return -1;
  }

  return 0;
}

void addEpIntoEpSet(SEpSet *pEpSet, const char* fqdn, uint16_t port) {
  if (pEpSet == NULL || fqdn == NULL || strlen(fqdn) == 0) {
    return;
  }

  int32_t index = pEpSet->numOfEps;
  tstrncpy(pEpSet->eps[index].fqdn, fqdn, tListLen(pEpSet->eps[index].fqdn));
  pEpSet->eps[index].port = port;
  pEpSet->numOfEps += 1;
}

bool isEpsetEqual(const SEpSet *s1, const SEpSet *s2) {
  if (s1->numOfEps != s2->numOfEps || s1->inUse != s2->inUse) {
    return false;
  }

  for (int32_t i = 0; i < s1->numOfEps; i++) {
    if (s1->eps[i].port != s2->eps[i].port
        || strncmp(s1->eps[i].fqdn, s2->eps[i].fqdn, TSDB_FQDN_LEN) != 0)
      return false;
  }
  return true;
}

void updateEpSet_s(SCorEpSet *pEpSet, SEpSet *pNewEpSet) {
  taosCorBeginWrite(&pEpSet->version);
  pEpSet->epSet = *pNewEpSet;
  taosCorEndWrite(&pEpSet->version);
}

SEpSet getEpSet_s(SCorEpSet *pEpSet) {
  SEpSet ep = {0};
  taosCorBeginRead(&pEpSet->version);
  ep = pEpSet->epSet;
  taosCorEndRead(&pEpSet->version);

  return ep;
}

#define BitmapLen(_n)     (((_n) + ((1<<NBIT)-1)) >> NBIT)

int32_t colDataGetSize(const SColumnInfoData* pColumnInfoData, int32_t numOfRows) {
  ASSERT(pColumnInfoData != NULL);
  if (IS_VAR_DATA_TYPE(pColumnInfoData->info.type)) {
    return pColumnInfoData->varmeta.length;
  } else {
    return pColumnInfoData->info.bytes * numOfRows;
  }
}

void colDataTrim(SColumnInfoData* pColumnInfoData) {
  // TODO
}

int32_t colDataAppend(SColumnInfoData* pColumnInfoData, uint32_t currentRow, const char* pData, bool isNull) {
  ASSERT(pColumnInfoData != NULL);

  if (isNull) {
    // There is a placehold for each NULL value of binary or nchar type.
    if (IS_VAR_DATA_TYPE(pColumnInfoData->info.type)) {
      pColumnInfoData->varmeta.offset[currentRow] = -1; // it is a null value of VAR type.
    } else {
      colDataSetNull_f(pColumnInfoData->nullbitmap, currentRow);
    }

    return 0;
  }

  int32_t type = pColumnInfoData->info.type;
  if (IS_VAR_DATA_TYPE(type)) {
    SVarColAttr* pAttr = &pColumnInfoData->varmeta;
    if (pAttr->allocLen < pAttr->length + varDataTLen(pData)) {
      uint32_t newSize = pAttr->allocLen;
      if (newSize == 0) {
        newSize = 8;
      }

      while(newSize < pAttr->length + varDataTLen(pData)) {
        newSize = newSize * 1.5;
      }

      char* buf = realloc(pColumnInfoData->pData, newSize);
      if (buf == NULL) {
        return TSDB_CODE_OUT_OF_MEMORY;
      }

      pColumnInfoData->pData = buf;
      pAttr->allocLen = newSize;
    }

    uint32_t len = pColumnInfoData->varmeta.length;
    pColumnInfoData->varmeta.offset[currentRow] = len;

    memcpy(pColumnInfoData->pData + len, pData, varDataTLen(pData));
    pColumnInfoData->varmeta.length += varDataTLen(pData);
  } else {
    char* p = pColumnInfoData->pData + pColumnInfoData->info.bytes * currentRow;
    switch(type) {
      case TSDB_DATA_TYPE_TINYINT:
      case TSDB_DATA_TYPE_UTINYINT: {*(int8_t*) p = *(int8_t*) pData;break;}
      case TSDB_DATA_TYPE_SMALLINT:
      case TSDB_DATA_TYPE_USMALLINT: {*(int16_t*) p = *(int16_t*) pData;break;}
      case TSDB_DATA_TYPE_INT:
      case TSDB_DATA_TYPE_UINT: {*(int32_t*) p = *(int32_t*) pData;break;}
      case TSDB_DATA_TYPE_BIGINT:
      case TSDB_DATA_TYPE_UBIGINT: {*(int64_t*) p = *(int64_t*) pData;break;}
      default:
        assert(0);
    }
  }

  return 0;
}

static void doBitmapMerge(SColumnInfoData* pColumnInfoData, int32_t numOfRow1, const SColumnInfoData* pSource, int32_t numOfRow2) {
  uint32_t total = numOfRow1 + numOfRow2;

  if (BitmapLen(numOfRow1) < BitmapLen(total)) {
    char*    tmp = realloc(pColumnInfoData->nullbitmap, BitmapLen(total));
    uint32_t extend = BitmapLen(total) - BitmapLen(numOfRow1);
    memset(tmp + BitmapLen(numOfRow1), 0, extend);
    pColumnInfoData->nullbitmap = tmp;
  }

  uint32_t remindBits = BitPos(numOfRow1);
  uint32_t shiftBits = 8 - remindBits;

  if (remindBits == 0) {  // no need to shift bits of bitmap
    memcpy(pColumnInfoData->nullbitmap + BitmapLen(numOfRow1), pSource->nullbitmap, BitmapLen(numOfRow2));
  } else {
    int32_t len = BitmapLen(numOfRow2);
    int32_t i = 0;

    uint8_t* p = (uint8_t*)pSource->nullbitmap;
    pColumnInfoData->nullbitmap[BitmapLen(numOfRow1) - 1] |= (p[0] >> remindBits);

    uint8_t* start = (uint8_t*)&pColumnInfoData->nullbitmap[BitmapLen(numOfRow1)];
    while (i < len) {
      start[i] |= (p[i] << shiftBits);
      i += 1;

      if (i > 1) {
        start[i - 1] |= (p[i] >> remindBits);
      }
    }
  }
}

int32_t colDataMergeCol(SColumnInfoData* pColumnInfoData, uint32_t numOfRow1, const SColumnInfoData* pSource, uint32_t numOfRow2) {
  ASSERT(pColumnInfoData != NULL && pSource != NULL && pColumnInfoData->info.type == pSource->info.type);

  if (numOfRow2 == 0) {
    return numOfRow1;
  }

  if (IS_VAR_DATA_TYPE(pColumnInfoData->info.type)) {
    // Handle the bitmap
    char* p = realloc(pColumnInfoData->varmeta.offset, sizeof(int32_t) * (numOfRow1 + numOfRow2));
    if (p == NULL) {
      // TODO
    }

    pColumnInfoData->varmeta.offset = (int32_t*) p;
    for(int32_t i = 0; i < numOfRow2; ++i) {
      pColumnInfoData->varmeta.offset[i + numOfRow1] = pSource->varmeta.offset[i] + pColumnInfoData->varmeta.length;
    }

    // copy data
    uint32_t len = pSource->varmeta.length;
    uint32_t oldLen = pColumnInfoData->varmeta.length;
    if (pColumnInfoData->varmeta.allocLen < len + oldLen) {
      char* tmp = realloc(pColumnInfoData->pData, len + oldLen);
      if (tmp == NULL) {
        return TSDB_CODE_VND_OUT_OF_MEMORY;
      }

      pColumnInfoData->pData = tmp;
      pColumnInfoData->varmeta.allocLen = len + oldLen;
    }

    memcpy(pColumnInfoData->pData + oldLen, pSource->pData, len);
    pColumnInfoData->varmeta.length = len + oldLen;
  } else {
    doBitmapMerge(pColumnInfoData, numOfRow1, pSource, numOfRow2);

    int32_t newSize = (numOfRow1 + numOfRow2) * pColumnInfoData->info.bytes;
    char*   tmp = realloc(pColumnInfoData->pData, newSize);
    if (tmp == NULL) {
      return TSDB_CODE_VND_OUT_OF_MEMORY;
    }

    pColumnInfoData->pData = tmp;
    int32_t offset = pColumnInfoData->info.bytes * numOfRow1;
    memcpy(pColumnInfoData->pData + offset, pSource->pData, pSource->info.bytes * numOfRow2);
  }

  return numOfRow1 + numOfRow2;
}

size_t colDataGetNumOfCols(const SSDataBlock* pBlock) {
  ASSERT(pBlock);

  size_t constantCols = (pBlock->pConstantList != NULL)? taosArrayGetSize(pBlock->pConstantList):0;
  ASSERT( pBlock->info.numOfCols == taosArrayGetSize(pBlock->pDataBlock) + constantCols);
  return pBlock->info.numOfCols;
}

size_t colDataGetNumOfRows(const SSDataBlock* pBlock) {
  return pBlock->info.rows;
}

int32_t colDataUpdateTsWindow(SSDataBlock* pDataBlock) {
  if (pDataBlock == NULL || pDataBlock->info.rows <= 0) {
    return 0;
  }

  if (pDataBlock->info.numOfCols <= 0) {
    return -1;
  }

  SColumnInfoData* pColInfoData = taosArrayGet(pDataBlock->pDataBlock, 0);
  if (pColInfoData->info.type != TSDB_DATA_TYPE_TIMESTAMP) {
    return 0;
  }

  ASSERT(pColInfoData->nullbitmap == NULL);
  pDataBlock->info.window.skey = *(TSKEY*) colDataGet(pColInfoData, 0);
  pDataBlock->info.window.ekey = *(TSKEY*) colDataGet(pColInfoData, (pDataBlock->info.rows - 1));
  return 0;
}

int32_t blockDataMerge(SSDataBlock* pDest, const SSDataBlock* pSrc) {
  assert(pSrc != NULL && pDest != NULL && pDest->info.numOfCols == pSrc->info.numOfCols);

  int32_t numOfCols = pSrc->info.numOfCols;
  for(int32_t i = 0; i < numOfCols; ++i) {
    SColumnInfoData* pCol2 = taosArrayGet(pDest->pDataBlock, i);
    SColumnInfoData* pCol1 = taosArrayGet(pSrc->pDataBlock, i);

    uint32_t oldLen = colDataGetSize(pCol2, pDest->info.rows);
    uint32_t newLen = colDataGetSize(pCol1, pSrc->info.rows);

    int32_t newSize = oldLen + newLen;
    char* tmp = realloc(pCol2->pData, newSize);
    if (tmp != NULL) {
      pCol2->pData = tmp;
      colDataMergeCol(pCol2, pDest->info.rows, pCol1, pSrc->info.rows);
    } else {
      return TSDB_CODE_VND_OUT_OF_MEMORY;
    }
  }

  pDest->info.rows += pSrc->info.rows;
  return TSDB_CODE_SUCCESS;
}

size_t blockDataGetSize(const SSDataBlock* pBlock) {
  assert(pBlock != NULL);

  size_t total = 0;
  int32_t numOfCols = pBlock->info.numOfCols;

  for(int32_t i = 0; i < numOfCols; ++i) {
    SColumnInfoData* pColInfoData = taosArrayGet(pBlock->pDataBlock, i);
    total += colDataGetSize(pColInfoData, pBlock->info.rows);

    if (IS_VAR_DATA_TYPE(pColInfoData->info.type)) {
      total += sizeof(int32_t) * pBlock->info.rows;
    } else {
      total += BitmapLen(pBlock->info.rows);
    }
  }

  return total;
}

// the number of tuples can be fit in one page.
// Actual data rows pluses the corresponding meta data must fit in one memory buffer of the given page size.
int32_t blockDataSplitRows(SSDataBlock* pBlock, bool hasVarCol, int32_t startIndex, int32_t* stopIndex, int32_t pageSize) {
  ASSERT(pBlock != NULL && stopIndex != NULL);

  int32_t numOfCols = pBlock->info.numOfCols;
  int32_t numOfRows = pBlock->info.rows;

  int32_t bitmapChar = 1;

  size_t headerSize    = sizeof(int32_t);
  size_t colHeaderSize = sizeof(int32_t) * numOfCols;
  size_t payloadSize   = pageSize - (headerSize + colHeaderSize);

  // TODO speedup by checking if the whole page can fit in firstly.
  if (!hasVarCol) {
    size_t rowSize = blockDataGetRowSize(pBlock);
    int32_t capacity = (payloadSize / (rowSize * 8 + bitmapChar * numOfCols)) * 8;

    *stopIndex = startIndex + capacity;
    if (*stopIndex >= numOfRows) {
      *stopIndex = numOfRows - 1;
    }

    return TSDB_CODE_SUCCESS;
  } else {
    // iterate the rows that can be fit in this buffer page
    int32_t size = (headerSize + colHeaderSize);

    for(int32_t j = startIndex; j < numOfRows; ++j) {
      for (int32_t i = 0; i < numOfCols; ++i) {
        SColumnInfoData* pColInfoData = TARRAY_GET_ELEM(pBlock->pDataBlock, i);
        if (IS_VAR_DATA_TYPE(pColInfoData->info.type)) {
          bool isNull = colDataIsNull(pColInfoData, numOfRows, j, NULL);
          if (isNull) {
            // do nothing
          } else {
            char* p = colDataGet(pColInfoData, j);
            size += varDataTLen(p);
          }

          size += sizeof(pColInfoData->varmeta.offset[0]);
        } else {
          size += pColInfoData->info.bytes;

          if (((j - startIndex) & 0x07) == 0) {
            size += 1; // the space for null bitmap
          }
        }
      }

      if (size > pageSize) {
        *stopIndex = j - 1;
        ASSERT(*stopIndex > startIndex);

        return TSDB_CODE_SUCCESS;
      }
    }

    // all fit in
    *stopIndex = numOfRows - 1;
    return TSDB_CODE_SUCCESS;
  }
}

SSDataBlock* blockDataExtractBlock(SSDataBlock* pBlock, int32_t startIndex, int32_t rowCount) {
  if (pBlock == NULL || startIndex < 0 || rowCount > pBlock->info.rows || rowCount + startIndex > pBlock->info.rows) {
    return NULL;
  }

  SSDataBlock* pDst = calloc(1, sizeof(SSDataBlock));
  if (pDst == NULL) {
    return NULL;
  }

  pDst->info = pBlock->info;

  pDst->info.rows = 0;
  pDst->pDataBlock = taosArrayInit(pBlock->info.numOfCols, sizeof(SColumnInfoData));

  for(int32_t i = 0; i < pBlock->info.numOfCols; ++i) {
    SColumnInfoData colInfo = {0};
    SColumnInfoData* pSrcCol = taosArrayGet(pBlock->pDataBlock, i);
    colInfo.info = pSrcCol->info;

    if (IS_VAR_DATA_TYPE(pSrcCol->info.type)) {
      SVarColAttr* pAttr = &colInfo.varmeta;
      pAttr->offset = calloc(rowCount, sizeof(int32_t));
    } else {
      colInfo.nullbitmap = calloc(1, BitmapLen(rowCount));
      colInfo.pData = calloc(rowCount, colInfo.info.bytes);
    }

    taosArrayPush(pDst->pDataBlock, &colInfo);
  }

  for (int32_t i = 0; i < pBlock->info.numOfCols; ++i) {
    SColumnInfoData* pColData = taosArrayGet(pBlock->pDataBlock, i);
    SColumnInfoData* pDstCol = taosArrayGet(pDst->pDataBlock, i);

    for (int32_t j = startIndex; j < (startIndex + rowCount); ++j) {
      bool isNull = colDataIsNull(pColData, pBlock->info.rows, j, pBlock->pBlockAgg);
      char* p = colDataGet(pColData, j);

      colDataAppend(pDstCol, j - startIndex, p, isNull);
    }
  }

  pDst->info.rows = rowCount;
  return pDst;
}


/**
 *
 * +------------------+---------------+--------------------+
 * |the number of rows| column length |     column #1      |
 * |    (4 bytes)     |  (4 bytes)    |--------------------+
 * |                  |               | null bitmap| values|
 * +------------------+---------------+--------------------+
 * @param buf
 * @param pBlock
 * @return
 */
int32_t blockDataToBuf(char* buf, const SSDataBlock* pBlock) {
  ASSERT(pBlock != NULL);

  // write the number of rows
  *(uint32_t*) buf = pBlock->info.rows;

  int32_t numOfCols = pBlock->info.numOfCols;
  int32_t numOfRows = pBlock->info.rows;

  char* pStart = buf + sizeof(uint32_t);

  for(int32_t i = 0; i < numOfCols; ++i) {
    SColumnInfoData* pCol = taosArrayGet(pBlock->pDataBlock, i);
    if (IS_VAR_DATA_TYPE(pCol->info.type)) {
      memcpy(pStart, pCol->varmeta.offset, numOfRows * sizeof(int32_t));
      pStart += numOfRows * sizeof(int32_t);
    } else {
      memcpy(pStart, pCol->nullbitmap, BitmapLen(numOfRows));
      pStart += BitmapLen(pBlock->info.rows);
    }

    uint32_t dataSize = colDataGetSize(pCol, numOfRows);

    *(int32_t*) pStart = dataSize;
    pStart += sizeof(int32_t);

    memcpy(pStart, pCol->pData, dataSize);
    pStart += dataSize;
  }

  return 0;
}

int32_t blockDataFromBuf(SSDataBlock* pBlock, const char* buf) {
  pBlock->info.rows = *(int32_t*) buf;

  int32_t numOfCols = pBlock->info.numOfCols;
  const char* pStart = buf + sizeof(uint32_t);

  for(int32_t i = 0; i < numOfCols; ++i) {
    SColumnInfoData* pCol = taosArrayGet(pBlock->pDataBlock, i);

    size_t metaSize = pBlock->info.rows * sizeof(int32_t);
    if (IS_VAR_DATA_TYPE(pCol->info.type)) {
      memcpy(pCol->varmeta.offset, pStart, metaSize);
      pStart += metaSize;
    } else {
      memcpy(pCol->nullbitmap, pStart, BitmapLen(pBlock->info.rows));
      pStart += BitmapLen(pBlock->info.rows);
    }

    int32_t colLength = *(int32_t*) pStart;
    pStart += sizeof(int32_t);

    if (IS_VAR_DATA_TYPE(pCol->info.type)) {
      if (pCol->varmeta.allocLen < colLength) {
        char* tmp = realloc(pCol->pData, colLength);
        if (tmp == NULL) {
          return TSDB_CODE_OUT_OF_MEMORY;
        }

        pCol->pData = tmp;
        pCol->varmeta.allocLen = colLength;
      }

      pCol->varmeta.length = colLength;
      ASSERT(pCol->varmeta.length <= pCol->varmeta.allocLen);
    }

    memcpy(pCol->pData, pStart, colLength);
    pStart += colLength;
  }

  return TSDB_CODE_SUCCESS;
}

size_t blockDataGetRowSize(const SSDataBlock* pBlock) {
  ASSERT(pBlock != NULL);
  size_t rowSize = 0;

  size_t numOfCols = pBlock->info.numOfCols;
  for(int32_t i = 0; i < numOfCols; ++i) {
    SColumnInfoData* pColInfo = taosArrayGet(pBlock->pDataBlock, i);
    rowSize += pColInfo->info.bytes;
  }

  return rowSize;
}

typedef struct SSDataBlockSortHelper {
  SArray      *orderInfo;   // SArray<SBlockOrderInfo>
  SSDataBlock *pDataBlock;
  bool         nullFirst;
} SSDataBlockSortHelper;

int32_t dataBlockCompar(const void* p1, const void* p2, const void* param) {
  const SSDataBlockSortHelper* pHelper = (const SSDataBlockSortHelper*) param;

  SSDataBlock* pDataBlock = pHelper->pDataBlock;

  int32_t left  = *(int32_t*) p1;
  int32_t right = *(int32_t*) p2;

  SArray* pInfo = pHelper->orderInfo;

  for(int32_t i = 0; i < pInfo->size; ++i) {
    SBlockOrderInfo* pOrder = TARRAY_GET_ELEM(pInfo, i);
    SColumnInfoData* pColInfoData = pOrder->pColData;//TARRAY_GET_ELEM(pDataBlock->pDataBlock, pOrder->colIndex);

    if (pColInfoData->hasNull) {
      bool leftNull  = colDataIsNull(pColInfoData, pDataBlock->info.rows, left, pDataBlock->pBlockAgg);
      bool rightNull = colDataIsNull(pColInfoData, pDataBlock->info.rows, right, pDataBlock->pBlockAgg);
      if (leftNull && rightNull) {
        continue; // continue to next slot
      }

      if (rightNull) {
        return pHelper->nullFirst? 1:-1;
      }

      if (leftNull) {
        return pHelper->nullFirst? -1:1;
      }
    }

    void* left1  = colDataGet(pColInfoData, left);
    void* right1 = colDataGet(pColInfoData, right);

    switch(pColInfoData->info.type) {
      case TSDB_DATA_TYPE_INT: {
        int32_t leftx  = *(int32_t*) left1;
        int32_t rightx = *(int32_t*) right1;

        if (leftx == rightx) {
          break;
        } else {
          if (pOrder->order == TSDB_ORDER_ASC) {
            return (leftx < rightx)? -1:1;
          } else {
            return (leftx < rightx)? 1:-1;
          }
        }
      }
      default:
        assert(0);
    }
  }

  return 0;
}

static int32_t doAssignOneTuple(SColumnInfoData* pDstCols, int32_t numOfRows, const SSDataBlock* pSrcBlock, int32_t tupleIndex) {
  int32_t code = 0;
  int32_t numOfCols = pSrcBlock->info.numOfCols;

  for (int32_t i = 0; i < numOfCols; ++i) {
    SColumnInfoData* pDst = &pDstCols[i];
    SColumnInfoData* pSrc = taosArrayGet(pSrcBlock->pDataBlock, i);

    if (pSrc->hasNull && colDataIsNull(pSrc, pSrcBlock->info.rows, tupleIndex, pSrcBlock->pBlockAgg)) {
      code = colDataAppend(pDst, numOfRows, NULL, true);
      if (code != TSDB_CODE_SUCCESS) {
        return code;
      }
    } else {
      char* p = colDataGet(pSrc, tupleIndex);
      code = colDataAppend(pDst, numOfRows, p, false);
      if (code != TSDB_CODE_SUCCESS) {
        return code;
      }
    }
  }

  return TSDB_CODE_SUCCESS;
}

static int32_t blockDataAssign(SColumnInfoData* pCols, const SSDataBlock* pDataBlock, int32_t* index) {
#if 0
  for (int32_t i = 0; i < pDataBlock->info.rows; ++i) {
    int32_t code = doAssignOneTuple(pCols, i, pDataBlock, index[i]);
    if (code != TSDB_CODE_SUCCESS) {
      return code;
    }
  }
#else
  for(int32_t i = 0; i < pDataBlock->info.numOfCols; ++i) {
    SColumnInfoData* pDst = &pCols[i];
    SColumnInfoData* pSrc = taosArrayGet(pDataBlock->pDataBlock, i);

    if (IS_VAR_DATA_TYPE(pSrc->info.type)) {
     memcpy(pDst->pData, pSrc->pData, pSrc->varmeta.length);
     pDst->varmeta.length = pSrc->varmeta.length;

     for(int32_t j = 0; j < pDataBlock->info.rows; ++j) {
       pDst->varmeta.offset[j] = pSrc->varmeta.offset[index[j]];
     }
    } else {
      switch (pSrc->info.type) {
        case TSDB_DATA_TYPE_UINT:
        case TSDB_DATA_TYPE_INT: {
          for (int32_t j = 0; j < pDataBlock->info.rows; ++j) {
            int32_t* p = (int32_t*)pDst->pData;
            int32_t* srclist = (int32_t*)pSrc->pData;

            p[j] = srclist[index[j]];
            if (colDataIsNull_f(pSrc->nullbitmap, index[j])) {
              colDataSetNull_f(pDst->nullbitmap, j);
            }
          }
          break;
        }
        case TSDB_DATA_TYPE_UTINYINT:
        case TSDB_DATA_TYPE_TINYINT: {
          for (int32_t j = 0; j < pDataBlock->info.rows; ++j) {
            int32_t* p = (int32_t*)pDst->pData;
            int32_t* srclist = (int32_t*)pSrc->pData;

            p[j] = srclist[index[j]];
            if (colDataIsNull_f(pSrc->nullbitmap, index[j])) {
              colDataSetNull_f(pDst->nullbitmap, j);
            }
          }
          break;
        }
        case TSDB_DATA_TYPE_USMALLINT:
        case TSDB_DATA_TYPE_SMALLINT: {
          for (int32_t j = 0; j < pDataBlock->info.rows; ++j) {
            int32_t* p = (int32_t*)pDst->pData;
            int32_t* srclist = (int32_t*)pSrc->pData;

            p[j] = srclist[index[j]];
            if (colDataIsNull_f(pSrc->nullbitmap, index[j])) {
              colDataSetNull_f(pDst->nullbitmap, j);
            }
          }
          break;
        }
        case TSDB_DATA_TYPE_UBIGINT:
        case TSDB_DATA_TYPE_BIGINT: {
          for (int32_t j = 0; j < pDataBlock->info.rows; ++j) {
            int32_t* p = (int32_t*)pDst->pData;
            int32_t* srclist = (int32_t*)pSrc->pData;

            p[j] = srclist[index[j]];
            if (colDataIsNull_f(pSrc->nullbitmap, index[j])) {
              colDataSetNull_f(pDst->nullbitmap, j);
            }
          }
          break;
        }
        default:
          assert(0);
      }
    }
  }
#endif
  return TSDB_CODE_SUCCESS;
}

static SColumnInfoData* createHelpColInfoData(const SSDataBlock* pDataBlock) {
  int32_t rows = pDataBlock->info.rows;
  int32_t numOfCols = pDataBlock->info.numOfCols;

  SColumnInfoData* pCols = calloc(numOfCols, sizeof(SColumnInfoData));
  if (pCols == NULL) {
    return NULL;
  }

  for(int32_t i = 0; i < numOfCols; ++i) {
    SColumnInfoData* pColInfoData = taosArrayGet(pDataBlock->pDataBlock, i);
    pCols[i].info = pColInfoData->info;

    if (IS_VAR_DATA_TYPE(pCols[i].info.type)) {
      pCols[i].varmeta.offset = calloc(rows, sizeof(int32_t));
      pCols[i].pData = calloc(1, pColInfoData->varmeta.length);

      pCols[i].varmeta.length = pColInfoData->varmeta.length;
      pCols[i].varmeta.allocLen = pCols[i].varmeta.length;
    } else {
      pCols[i].nullbitmap = calloc(1, BitmapLen(rows));
      pCols[i].pData = calloc(rows, pCols[i].info.bytes);
    }
  }

  return pCols;
}

static void copyBackToBlock(SSDataBlock* pDataBlock, SColumnInfoData* pCols) {
  int32_t numOfCols = pDataBlock->info.numOfCols;

  for(int32_t i = 0; i < numOfCols; ++i) {
    SColumnInfoData* pColInfoData = taosArrayGet(pDataBlock->pDataBlock, i);
    pColInfoData->info = pCols[i].info;

    if (IS_VAR_DATA_TYPE(pColInfoData->info.type)) {
      tfree(pColInfoData->varmeta.offset);
      pColInfoData->varmeta = pCols[i].varmeta;
    } else {
      tfree(pColInfoData->nullbitmap);
      pColInfoData->nullbitmap = pCols[i].nullbitmap;
    }

    tfree(pColInfoData->pData);
    pColInfoData->pData = pCols[i].pData;
  }

  tfree(pCols);
}

static int32_t* createTupleIndex(size_t rows) {
  int32_t* index = calloc(rows, sizeof(int32_t));
  if (index == NULL) {
    return NULL;
  }

  for(int32_t i = 0; i < rows; ++i) {
    index[i] = i;
  }

  return index;
}

static void destroyTupleIndex(int32_t* index) {
  tfree(index);
}

int32_t blockDataSort(SSDataBlock* pDataBlock, SArray* pOrderInfo, bool nullFirst) {
  ASSERT(pDataBlock != NULL && pOrderInfo != NULL);
  if (pDataBlock->info.rows <= 1) {
    return TSDB_CODE_SUCCESS;
  }

  // Allocate the additional buffer.
  uint32_t rows = pDataBlock->info.rows;
  int32_t* index = createTupleIndex(rows);
  if (index == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return terrno;
  }

  int64_t p0 = taosGetTimestampUs();

  SSDataBlockSortHelper helper = {.nullFirst = nullFirst, .pDataBlock = pDataBlock, .orderInfo = pOrderInfo};
  for(int32_t i = 0; i < taosArrayGetSize(helper.orderInfo); ++i) {
    struct SBlockOrderInfo* pInfo = taosArrayGet(helper.orderInfo, i);
    pInfo->pColData = taosArrayGet(pDataBlock->pDataBlock, pInfo->colIndex);
  }

  taosqsort(index, rows, sizeof(int32_t), &helper, dataBlockCompar);

  int64_t p1 = taosGetTimestampUs();

  SColumnInfoData* pCols = createHelpColInfoData(pDataBlock);
  if (pCols == NULL) {
    terrno = TSDB_CODE_OUT_OF_MEMORY;
    return terrno;
  }

#if 0
  SColumnInfoData* px = taosArrayGet(pDataBlock->pDataBlock, 0);
  for(int32_t i = 0; i < pDataBlock->info.rows; ++i) {
    printf("%d, %d, %d\n", index[i], ((int32_t*)px->pData)[i], ((int32_t*)px->pData)[index[i]]);
  }
#endif
  int64_t p2 = taosGetTimestampUs();

  int32_t code = blockDataAssign(pCols, pDataBlock, index);
  if (code != TSDB_CODE_SUCCESS) {
    terrno = code;
    return code;
  }

  int64_t p3 = taosGetTimestampUs();

#if 0
  for(int32_t i = 0; i < pDataBlock->info.rows; ++i) {
    if (colDataIsNull(&pCols[0], rows, i, NULL)) {
      printf("0\t");
    } else {
      printf("%d\t", ((int32_t*)pCols[0].pData)[i]);
    }
  }

  printf("end\n");
#endif

  copyBackToBlock(pDataBlock, pCols);
  int64_t p4 = taosGetTimestampUs();

  printf("sort:%ld, create:%ld, assign:%ld, copyback:%ld, rows:%d\n", p1-p0, p2 - p1, p3 - p2, p4-p3, rows);
  destroyTupleIndex(index);

  return TSDB_CODE_SUCCESS;
}

void blockDataClearup(SSDataBlock* pDataBlock, bool hasVarCol) {
  pDataBlock->info.rows = 0;

  if (hasVarCol) {
    for (int32_t i = 0; i < pDataBlock->info.numOfCols; ++i) {
      SColumnInfoData* p = taosArrayGet(pDataBlock->pDataBlock, i);

      if (IS_VAR_DATA_TYPE(p->info.type)) {
        p->varmeta.length = 0;
      }
    }
  }
}

int32_t blockDataEnsureCapacity(SSDataBlock* pDataBlock, uint32_t numOfRows) {
  for(int32_t i = 0; i < pDataBlock->info.numOfCols; ++i) {
    SColumnInfoData* p = taosArrayGet(pDataBlock->pDataBlock, i);
    if (IS_VAR_DATA_TYPE(p->info.type)) {
      char* tmp = realloc(p->varmeta.offset, sizeof(int32_t) * numOfRows);
      if (tmp == NULL) {
        return TSDB_CODE_OUT_OF_MEMORY;
      }

      p->varmeta.offset = (int32_t*)tmp;
      memset(p->varmeta.offset, 0, sizeof(int32_t) * numOfRows);

      p->varmeta.length = 0;
      p->varmeta.allocLen = 0;
      tfree(p->pData);
    } else {
      char* tmp = realloc(p->nullbitmap, BitmapLen(numOfRows));
      if (tmp == NULL) {
        return TSDB_CODE_OUT_OF_MEMORY;
      }

      p->nullbitmap = tmp;
      memset(p->nullbitmap, 0, BitmapLen(numOfRows));

      tmp = realloc(p->pData, numOfRows * p->info.bytes);
      if (tmp == NULL) {
        return TSDB_CODE_OUT_OF_MEMORY;
      }

      p->pData = tmp;
    }
  }

  return TSDB_CODE_SUCCESS;
}

void* blockDataDestroy(SSDataBlock* pBlock) {
  if (pBlock == NULL) {
    return NULL;
  }

  int32_t numOfOutput = pBlock->info.numOfCols;
  for(int32_t i = 0; i < numOfOutput; ++i) {
    SColumnInfoData* pColInfoData = taosArrayGet(pBlock->pDataBlock, i);
    if (IS_VAR_DATA_TYPE(pColInfoData->info.type)) {
      tfree(pColInfoData->varmeta.offset);
    } else {
      tfree(pColInfoData->nullbitmap);
    }

    tfree(pColInfoData->pData);
  }

  taosArrayDestroy(pBlock->pDataBlock);
  tfree(pBlock->pBlockAgg);
  tfree(pBlock);
  return NULL;
}