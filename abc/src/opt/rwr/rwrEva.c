/**CFile****************************************************************

  FileName    [rwrDec.c]

  SystemName  [ABC: Logic synthesis and verification system.]

  PackageName [DAG-aware AIG rewriting package.]

  Synopsis    [Evaluation and decomposition procedures.]

  Author      [Alan Mishchenko]
  
  Affiliation [UC Berkeley]

  Date        [Ver. 1.0. Started - June 20, 2005.]

  Revision    [$Id: rwrDec.c,v 1.00 2005/06/20 00:00:00 alanmi Exp $]

***********************************************************************/

#include "rwr.h"
#include "bool/dec/dec.h"
#include "aig/ivy/ivy.h"

ABC_NAMESPACE_IMPL_START


////////////////////////////////////////////////////////////////////////
///                        DECLARATIONS                              ///
////////////////////////////////////////////////////////////////////////

static Dec_Graph_t * Rwr_CutEvaluate( Rwr_Man_t * p, Abc_Obj_t * pRoot, Cut_Cut_t * pCut, Vec_Ptr_t * vFaninsCur, int nNodesSaved, int LevelMax, int * pGainBest, int fPlaceEnable );
static void Rwr_CutProfileEvaluate( Rwr_Man_t * p, Abc_Obj_t * pRoot, Cut_Cut_t * pCut, Vec_Ptr_t * vFaninsCur, int nNodesSaved, int LevelMax, Rwr_Profile_t * pProf, void * pRootProf );
static int Rwr_CutIsBoolean( Abc_Obj_t * pObj, Vec_Ptr_t * vLeaves );
static int Rwr_CutCountNumNodes( Abc_Obj_t * pObj, Cut_Cut_t * pCut );
static int Rwr_NodeGetDepth_rec( Abc_Obj_t * pObj, Vec_Ptr_t * vLeaves );

typedef struct Rwr_ProfileCand_t_ Rwr_ProfileCand_t;
typedef struct Rwr_ProfileRoot_t_ Rwr_ProfileRoot_t;

struct Rwr_ProfileCand_t_
{
    int          Gain;
    int          nNodesAdded;
    int          nGraphNodes;
    int          fSelected;
    int          Share;
    Vec_Int_t *  vAddedSigs;
};

struct Rwr_ProfileRoot_t_
{
    int                  RootId;
    int                  nCuts;
    int                  nCandidates;
    int                  nGainPos;
    int                  nGainZero;
    int                  nGainNeg;
    int                  BestGain;
    int                  SelectedShare;
    int                  MaxZeroShare;
    int                  fZeroBestDiffers;
    Rwr_ProfileCand_t *  pSelected;
    Vec_Ptr_t *          vCands;
};

struct Rwr_Profile_t_
{
    int          nKeepMax;
    int          fVerbose;
    Vec_Ptr_t *  vRoots;
    Vec_Int_t *  vSigType;
    Vec_Int_t *  vSigA;
    Vec_Int_t *  vSigB;
    Vec_Int_t *  vSigNext;
    Vec_Int_t *  vSigTable;
    Vec_Int_t *  vSigFreq;
    Vec_Int_t *  vSigRootFreq;
    Vec_Int_t *  vExistingSigs;
    Vec_Int_t *  vObjSigLit;
    int          nExistingSigs;
    long long    nRootsConsidered;
    long long    nCutsConsidered;
    long long    nCandidatesConsidered;
    long long    nGainPos;
    long long    nGainZero;
    long long    nGainNeg;
    long long    nRootsGainPos;
    long long    nRootsGainZero;
    long long    nRootsBestZero;
    long long    nRootsZeroAnalyzed;
    long long    nRootsZeroDiffers;
    long long    nTotalAddedSigOccs;
    long long    nUniqueAddedSigs;
    long long    nRepeatedAddedSigs;
    long long    nCrossRootRepeatedSigs;
    long long    nMaxSigFreq;
    double       dAvgRepeatedSigFreq;
};

static unsigned Rwr_ProfileSigHash( int Type, int A, int B, int nBins )
{
    unsigned Key = 2166136261u;
    Key = (Key ^ (unsigned)Type) * 16777619u;
    Key = (Key ^ (unsigned)A) * 16777619u;
    Key = (Key ^ (unsigned)B) * 16777619u;
    return Key % (unsigned)nBins;
}

static void Rwr_ProfileSigRehash( Rwr_Profile_t * p )
{
    int i, Entry, nBinsNew, * pPlace;
    nBinsNew = Abc_PrimeCudd( 2 * Vec_IntSize(p->vSigTable) + 1 );
    Vec_IntFill( p->vSigTable, nBinsNew, -1 );
    for ( i = 0; i < Vec_IntSize(p->vSigType); i++ )
    {
        pPlace = Vec_IntEntryP( p->vSigTable, Rwr_ProfileSigHash( Vec_IntEntry(p->vSigType, i), Vec_IntEntry(p->vSigA, i), Vec_IntEntry(p->vSigB, i), Vec_IntSize(p->vSigTable) ) );
        Entry = *pPlace;
        Vec_IntWriteEntry( p->vSigNext, i, Entry );
        *pPlace = i;
    }
}

static int Rwr_ProfileSigFindOrAdd( Rwr_Profile_t * p, int Type, int A, int B )
{
    int Entry, * pPlace;
    pPlace = Vec_IntEntryP( p->vSigTable, Rwr_ProfileSigHash( Type, A, B, Vec_IntSize(p->vSigTable) ) );
    for ( Entry = *pPlace; Entry >= 0; Entry = Vec_IntEntry(p->vSigNext, Entry) )
        if ( Vec_IntEntry(p->vSigType, Entry) == Type && Vec_IntEntry(p->vSigA, Entry) == A && Vec_IntEntry(p->vSigB, Entry) == B )
            return Entry;
    Entry = Vec_IntSize( p->vSigType );
    Vec_IntPush( p->vSigType, Type );
    Vec_IntPush( p->vSigA, A );
    Vec_IntPush( p->vSigB, B );
    Vec_IntPush( p->vSigNext, *pPlace );
    Vec_IntPush( p->vSigFreq, 0 );
    Vec_IntPush( p->vSigRootFreq, 0 );
    Vec_IntPush( p->vExistingSigs, 0 );
    *pPlace = Entry;
    if ( Vec_IntSize(p->vSigType) > Vec_IntSize(p->vSigTable) )
        Rwr_ProfileSigRehash( p );
    return Entry;
}

static int Rwr_ProfileLeafLit( Rwr_Profile_t * p, int ObjId, int fCompl )
{
    return Abc_Var2Lit( Rwr_ProfileSigFindOrAdd( p, 0, ObjId, 0 ), fCompl );
}

static int Rwr_ProfileAndSig( Rwr_Profile_t * p, int Lit0, int Lit1 )
{
    int Temp;
    if ( Lit0 > Lit1 )
    {
        Temp = Lit0;
        Lit0 = Lit1;
        Lit1 = Temp;
    }
    return Rwr_ProfileSigFindOrAdd( p, 1, Lit0, Lit1 );
}

static void Rwr_ProfileMarkExistingSig( Rwr_Profile_t * p, int Sig )
{
    Vec_IntFillExtra( p->vExistingSigs, Sig + 1, 0 );
    if ( Vec_IntEntry(p->vExistingSigs, Sig) == 0 )
    {
        Vec_IntWriteEntry( p->vExistingSigs, Sig, 1 );
        p->nExistingSigs++;
    }
}

static int Rwr_ProfileObjLit_rec( Rwr_Profile_t * p, Abc_Obj_t * pObj )
{
    int Id, Lit0, Lit1, Sig;
    pObj = Abc_ObjRegular( pObj );
    Id = Abc_ObjId( pObj );
    Vec_IntFillExtra( p->vObjSigLit, Id + 1, -1 );
    if ( Vec_IntEntry(p->vObjSigLit, Id) >= 0 )
        return Vec_IntEntry( p->vObjSigLit, Id );
    if ( !Abc_ObjIsNode(pObj) )
    {
        Lit0 = Rwr_ProfileLeafLit( p, Id, 0 );
        Vec_IntWriteEntry( p->vObjSigLit, Id, Lit0 );
        return Lit0;
    }
    Lit0 = Abc_LitNotCond( Rwr_ProfileObjLit_rec(p, Abc_ObjFanin0(pObj)), Abc_ObjFaninC0(pObj) );
    Lit1 = Abc_LitNotCond( Rwr_ProfileObjLit_rec(p, Abc_ObjFanin1(pObj)), Abc_ObjFaninC1(pObj) );
    Sig = Rwr_ProfileAndSig( p, Lit0, Lit1 );
    Lit0 = Abc_Var2Lit( Sig, 0 );
    Vec_IntWriteEntry( p->vObjSigLit, Id, Lit0 );
    return Lit0;
}

static void Rwr_ProfileBuildExistingSigs( Rwr_Profile_t * p, Abc_Ntk_t * pNtk )
{
    Abc_Obj_t * pObj;
    int i, Lit0, Lit1, Sig;
    Abc_NtkForEachNode( pNtk, pObj, i )
    {
        Sig = Abc_Lit2Var( Rwr_ProfileObjLit_rec( p, pObj ) );
        Rwr_ProfileMarkExistingSig( p, Sig );
        Lit0 = Rwr_ProfileLeafLit( p, Abc_ObjId(Abc_ObjFanin0(pObj)), Abc_ObjFaninC0(pObj) );
        Lit1 = Rwr_ProfileLeafLit( p, Abc_ObjId(Abc_ObjFanin1(pObj)), Abc_ObjFaninC1(pObj) );
        Rwr_ProfileMarkExistingSig( p, Rwr_ProfileAndSig( p, Lit0, Lit1 ) );
    }
}

Vec_Int_t * Rwr_ProfileGraphAddedSigs( Rwr_Profile_t * p, Dec_Graph_t * pGraph, Vec_Ptr_t * vFaninsCur )
{
    Vec_Int_t * vNodeLits, * vSigs;
    Dec_Node_t * pNode;
    Abc_Obj_t * pFanin;
    int i, Lit0, Lit1, Sig;
    vSigs = Vec_IntAlloc( Dec_GraphNodeNum(pGraph) );
    if ( Dec_GraphIsConst(pGraph) || Dec_GraphIsVar(pGraph) )
        return vSigs;
    vNodeLits = Vec_IntStartFull( pGraph->nSize );
    Dec_GraphForEachLeaf( pGraph, pNode, i )
    {
        pFanin = (Abc_Obj_t *)Vec_PtrEntry( vFaninsCur, i );
        Vec_IntWriteEntry( vNodeLits, i, Rwr_ProfileLeafLit( p, Abc_ObjId(Abc_ObjRegular(pFanin)), Abc_ObjIsComplement(pFanin) ) );
    }
    Dec_GraphForEachNode( pGraph, pNode, i )
    {
        Lit0 = Abc_LitNotCond( Vec_IntEntry(vNodeLits, pNode->eEdge0.Node), pNode->eEdge0.fCompl );
        Lit1 = Abc_LitNotCond( Vec_IntEntry(vNodeLits, pNode->eEdge1.Node), pNode->eEdge1.fCompl );
        Sig = Rwr_ProfileAndSig( p, Lit0, Lit1 );
        Vec_IntWriteEntry( vNodeLits, i, Abc_Var2Lit(Sig, 0) );
        if ( Sig >= Vec_IntSize(p->vExistingSigs) || Vec_IntEntry(p->vExistingSigs, Sig) == 0 )
            Vec_IntPushUnique( vSigs, Sig );
    }
    Vec_IntFree( vNodeLits );
    return vSigs;
}

int Rwr_ProfileObjSigLit( Rwr_Profile_t * pProf, Abc_Obj_t * pObj )
{
    if ( pProf == NULL || pObj == NULL )
        return -1;
    return Rwr_ProfileObjLit_rec( pProf, pObj );
}

int Rwr_ProfileSigFreq( Rwr_Profile_t * p, int Sig )
{
    if ( Sig < 0 || Sig >= Vec_IntSize(p->vSigFreq) )
        return 0;
    return Vec_IntEntry( p->vSigFreq, Sig );
}

int Rwr_ProfileSigRootFreq( Rwr_Profile_t * p, int Sig )
{
    if ( Sig < 0 || Sig >= Vec_IntSize(p->vSigRootFreq) )
        return 0;
    return Vec_IntEntry( p->vSigRootFreq, Sig );
}

int Rwr_ProfileSigFreqByLit( Rwr_Profile_t * p, int SigLit )
{
    if ( SigLit < 0 )
        return 0;
    return Rwr_ProfileSigFreq( p, Abc_Lit2Var(SigLit) );
}

int Rwr_ProfileSigRootFreqByLit( Rwr_Profile_t * p, int SigLit )
{
    if ( SigLit < 0 )
        return 0;
    return Rwr_ProfileSigRootFreq( p, Abc_Lit2Var(SigLit) );
}

int Rwr_ProfileScoreAddedSigs( Rwr_Profile_t * p, Vec_Int_t * vAddedSigs )
{
    int i, Sig, Freq, Score = 0;
    Vec_IntForEachEntry( vAddedSigs, Sig, i )
    {
        Freq = Rwr_ProfileSigFreq( p, Sig );
        if ( Freq > 1 )
            Score += Freq - 1;
    }
    return Score;
}

int Rwr_ProfileScoreAddedSigsMode( Rwr_Profile_t * p, Vec_Int_t * vAddedSigs, int ExistingReuse, int nScoreMode )
{
    int i, Sig, Freq, Score = 0, RawRootShare = 0, AddedCount;
    if ( nScoreMode == 1 )
        return Rwr_ProfileScoreAddedSigs( p, vAddedSigs );
    Vec_IntForEachEntry( vAddedSigs, Sig, i )
    {
        Freq = Rwr_ProfileSigRootFreq( p, Sig );
        if ( Freq > 1 )
            RawRootShare += Freq - 1;
    }
    if ( nScoreMode == 2 )
        return RawRootShare;
    if ( nScoreMode == 3 )
    {
        AddedCount = Vec_IntSize( vAddedSigs );
        Score = (100 * RawRootShare) / Abc_MaxInt( 1, AddedCount );
        Score += 20 * ExistingReuse;
        return Score;
    }
    return Rwr_ProfileScoreAddedSigs( p, vAddedSigs );
}

long long Rwr_ProfileRepeatedSigs( Rwr_Profile_t * p )
{
    return p->nRepeatedAddedSigs;
}

long long Rwr_ProfileCrossRootSigs( Rwr_Profile_t * p )
{
    return p->nCrossRootRepeatedSigs;
}

static Rwr_ProfileCand_t * Rwr_ProfileCandAlloc( int Gain, int nNodesAdded, Dec_Graph_t * pGraph, Vec_Int_t * vAddedSigs )
{
    Rwr_ProfileCand_t * pCand = ABC_CALLOC( Rwr_ProfileCand_t, 1 );
    pCand->Gain = Gain;
    pCand->nNodesAdded = nNodesAdded;
    pCand->nGraphNodes = Dec_GraphNodeNum( pGraph );
    pCand->Share = -1;
    pCand->vAddedSigs = vAddedSigs;
    return pCand;
}

static void Rwr_ProfileCandFree( Rwr_ProfileCand_t * pCand )
{
    if ( pCand == NULL )
        return;
    Vec_IntFree( pCand->vAddedSigs );
    ABC_FREE( pCand );
}

static int Rwr_ProfileCandBetter( Rwr_ProfileCand_t * pCand0, Rwr_ProfileCand_t * pCand1 )
{
    if ( pCand0->Gain != pCand1->Gain )
        return pCand0->Gain > pCand1->Gain;
    if ( pCand0->nGraphNodes != pCand1->nGraphNodes )
        return pCand0->nGraphNodes < pCand1->nGraphNodes;
    return pCand0->nNodesAdded < pCand1->nNodesAdded;
}

static void Rwr_ProfileRootAddCand( Rwr_Profile_t * p, Rwr_ProfileRoot_t * pRoot, Rwr_ProfileCand_t * pCand )
{
    Rwr_ProfileCand_t * pWorst;
    Rwr_ProfileCand_t * pEntry;
    int i, iWorst;
    if ( p->nKeepMax <= 0 )
    {
        Rwr_ProfileCandFree( pCand );
        return;
    }
    if ( Vec_PtrSize(pRoot->vCands) < p->nKeepMax )
    {
        Vec_PtrPush( pRoot->vCands, pCand );
        return;
    }
    iWorst = -1;
    pWorst = NULL;
    for ( i = 0; i < Vec_PtrSize(pRoot->vCands); i++ )
    {
        pEntry = (Rwr_ProfileCand_t *)Vec_PtrEntry( pRoot->vCands, i );
        if ( pEntry == pRoot->pSelected && pCand != pRoot->pSelected )
            continue;
        if ( pWorst == NULL || Rwr_ProfileCandBetter( pWorst, pEntry ) )
        {
            iWorst = i;
            pWorst = pEntry;
        }
    }
    if ( pWorst == NULL )
    {
        Rwr_ProfileCandFree( pCand );
        return;
    }
    if ( Rwr_ProfileCandBetter( pCand, pWorst ) )
    {
        Vec_PtrWriteEntry( pRoot->vCands, iWorst, pCand );
        Rwr_ProfileCandFree( pWorst );
    }
    else
        Rwr_ProfileCandFree( pCand );
}

static Rwr_ProfileRoot_t * Rwr_ProfileRootAlloc( int RootId )
{
    Rwr_ProfileRoot_t * pRoot = ABC_CALLOC( Rwr_ProfileRoot_t, 1 );
    pRoot->RootId = RootId;
    pRoot->BestGain = -1;
    pRoot->SelectedShare = -1;
    pRoot->MaxZeroShare = -1;
    pRoot->vCands = Vec_PtrAlloc( 8 );
    return pRoot;
}

static void Rwr_ProfileRootFree( Rwr_ProfileRoot_t * pRoot )
{
    Rwr_ProfileCand_t * pCand;
    int i;
    if ( pRoot == NULL )
        return;
    Vec_PtrForEachEntry( Rwr_ProfileCand_t *, pRoot->vCands, pCand, i )
        Rwr_ProfileCandFree( pCand );
    Vec_PtrFree( pRoot->vCands );
    ABC_FREE( pRoot );
}

Rwr_Profile_t * Rwr_ProfileStart( Abc_Ntk_t * pNtk, int nKeepMax, int fVerbose )
{
    Rwr_Profile_t * p = ABC_CALLOC( Rwr_Profile_t, 1 );
    int nTableSize = Abc_PrimeCudd( Abc_MaxInt( 1009, 2 * Abc_NtkObjNumMax(pNtk) + 101 ) );
    p->nKeepMax = nKeepMax;
    p->fVerbose = fVerbose;
    p->vRoots = Vec_PtrAlloc( 1024 );
    p->vSigType = Vec_IntAlloc( nTableSize );
    p->vSigA = Vec_IntAlloc( nTableSize );
    p->vSigB = Vec_IntAlloc( nTableSize );
    p->vSigNext = Vec_IntAlloc( nTableSize );
    p->vSigTable = Vec_IntStartFull( nTableSize );
    p->vSigFreq = Vec_IntAlloc( nTableSize );
    p->vSigRootFreq = Vec_IntAlloc( nTableSize );
    p->vExistingSigs = Vec_IntAlloc( nTableSize );
    p->vObjSigLit = Vec_IntStartFull( Abc_NtkObjNumMax(pNtk) );
    Rwr_ProfileBuildExistingSigs( p, pNtk );
    return p;
}

void Rwr_ProfileStop( Rwr_Profile_t * p )
{
    Rwr_ProfileRoot_t * pRoot;
    int i;
    if ( p == NULL )
        return;
    Vec_PtrForEachEntry( Rwr_ProfileRoot_t *, p->vRoots, pRoot, i )
        Rwr_ProfileRootFree( pRoot );
    Vec_PtrFree( p->vRoots );
    Vec_IntFree( p->vSigType );
    Vec_IntFree( p->vSigA );
    Vec_IntFree( p->vSigB );
    Vec_IntFree( p->vSigNext );
    Vec_IntFree( p->vSigTable );
    Vec_IntFree( p->vSigFreq );
    Vec_IntFree( p->vSigRootFreq );
    Vec_IntFree( p->vExistingSigs );
    Vec_IntFree( p->vObjSigLit );
    ABC_FREE( p );
}

void Rwr_ProfileFinalize( Rwr_Profile_t * p )
{
    Rwr_ProfileRoot_t * pRoot;
    Rwr_ProfileCand_t * pCand;
    Vec_Int_t * vRootSigs;
    int i, k, s, Freq;
    long long SumRepFreq;
    Vec_PtrForEachEntry( Rwr_ProfileRoot_t *, p->vRoots, pRoot, i )
    {
        Vec_PtrForEachEntry( Rwr_ProfileCand_t *, pRoot->vCands, pCand, k )
        {
            Vec_IntForEachEntry( pCand->vAddedSigs, s, Freq )
            {
                Vec_IntFillExtra( p->vSigFreq, s + 1, 0 );
                Vec_IntAddToEntry( p->vSigFreq, s, 1 );
                p->nTotalAddedSigOccs++;
            }
        }
    }
    Vec_PtrForEachEntry( Rwr_ProfileRoot_t *, p->vRoots, pRoot, i )
    {
        vRootSigs = Vec_IntAlloc( 16 );
        Vec_PtrForEachEntry( Rwr_ProfileCand_t *, pRoot->vCands, pCand, k )
            Vec_IntForEachEntry( pCand->vAddedSigs, s, Freq )
                Vec_IntPushUnique( vRootSigs, s );
        Vec_IntForEachEntry( vRootSigs, s, k )
        {
            Vec_IntFillExtra( p->vSigRootFreq, s + 1, 0 );
            Vec_IntAddToEntry( p->vSigRootFreq, s, 1 );
        }
        Vec_IntFree( vRootSigs );
    }
    SumRepFreq = 0;
    for ( i = 0; i < Vec_IntSize(p->vSigFreq); i++ )
    {
        Freq = Vec_IntEntry( p->vSigFreq, i );
        if ( Freq == 0 )
            continue;
        p->nUniqueAddedSigs++;
        if ( Freq > 1 )
        {
            p->nRepeatedAddedSigs++;
            SumRepFreq += Freq;
        }
        if ( p->nMaxSigFreq < Freq )
            p->nMaxSigFreq = Freq;
        if ( i < Vec_IntSize(p->vSigRootFreq) && Vec_IntEntry(p->vSigRootFreq, i) > 1 )
            p->nCrossRootRepeatedSigs++;
    }
    if ( p->nRepeatedAddedSigs )
        p->dAvgRepeatedSigFreq = 1.0 * SumRepFreq / p->nRepeatedAddedSigs;
    Vec_PtrForEachEntry( Rwr_ProfileRoot_t *, p->vRoots, pRoot, i )
    {
        pRoot->MaxZeroShare = -1;
        Vec_PtrForEachEntry( Rwr_ProfileCand_t *, pRoot->vCands, pCand, k )
        {
            pCand->Share = 0;
            Vec_IntForEachEntry( pCand->vAddedSigs, s, Freq )
                if ( Vec_IntEntry(p->vSigFreq, s) > 1 )
                    pCand->Share += Vec_IntEntry(p->vSigFreq, s) - 1;
            if ( pCand == pRoot->pSelected )
                pRoot->SelectedShare = pCand->Share;
            if ( pCand->Gain == 0 && pRoot->MaxZeroShare < pCand->Share )
                pRoot->MaxZeroShare = pCand->Share;
        }
        if ( pRoot->BestGain == 0 )
        {
            p->nRootsZeroAnalyzed++;
            if ( pRoot->SelectedShare >= 0 && pRoot->MaxZeroShare > pRoot->SelectedShare )
            {
                pRoot->fZeroBestDiffers = 1;
                p->nRootsZeroDiffers++;
            }
        }
    }
}

void Rwr_ProfilePrint( Rwr_Profile_t * p )
{
    Rwr_ProfileRoot_t * pRoot;
    int i;
    printf( "Rewrite profile summary:\n" );
    printf( "  Roots considered                 : %lld\n", p->nRootsConsidered );
    printf( "  Cuts considered                  : %lld\n", p->nCutsConsidered );
    printf( "  Candidates considered            : %lld\n", p->nCandidatesConsidered );
    printf( "\n" );
    printf( "  Gain > 0 candidates              : %lld\n", p->nGainPos );
    printf( "  Gain = 0 candidates              : %lld\n", p->nGainZero );
    printf( "  Gain < 0 candidates              : %lld\n", p->nGainNeg );
    printf( "\n" );
    printf( "  Roots with gain > 0 candidates   : %lld\n", p->nRootsGainPos );
    printf( "  Roots with gain = 0 candidates   : %lld\n", p->nRootsGainZero );
    printf( "  Roots with best gain = 0         : %lld\n", p->nRootsBestZero );
    printf( "\n" );
    printf( "  Existing AIG signatures          : %d\n", p->nExistingSigs );
    printf( "  Total added signatures           : %lld\n", p->nTotalAddedSigOccs );
    printf( "  Unique added signatures          : %lld\n", p->nUniqueAddedSigs );
    printf( "  Repeated added signatures        : %lld\n", p->nRepeatedAddedSigs );
    printf( "  Cross-root repeated signatures   : %lld\n", p->nCrossRootRepeatedSigs );
    printf( "  Max signature frequency          : %lld\n", p->nMaxSigFreq );
    printf( "  Average repeated signature frequency : %.2f\n", p->dAvgRepeatedSigFreq );
    printf( "\n" );
    printf( "  Zero-gain roots analyzed         : %lld\n", p->nRootsZeroAnalyzed );
    printf( "  Zero-gain roots where best-local differs from max-sharing : %lld\n", p->nRootsZeroDiffers );
    if ( !p->fVerbose )
        return;
    Vec_PtrForEachEntry( Rwr_ProfileRoot_t *, p->vRoots, pRoot, i )
    {
        if ( pRoot->BestGain == 0 )
            printf( "root=%d cuts=%d cands=%d best_gain=%d pos_cands=%d zero_cands=%d selected_share=%d max_share=%d differ=%d\n",
                pRoot->RootId, pRoot->nCuts, pRoot->nCandidates, pRoot->BestGain, pRoot->nGainPos, pRoot->nGainZero,
                pRoot->SelectedShare, pRoot->MaxZeroShare, pRoot->fZeroBestDiffers );
    }
}

////////////////////////////////////////////////////////////////////////
///                     FUNCTION DEFINITIONS                         ///
////////////////////////////////////////////////////////////////////////

/**Function*************************************************************

  Synopsis    [Performs rewriting for one node.]

  Description [This procedure considers all the cuts computed for the node
  and tries to rewrite each of them using the "forest" of different AIG
  structures precomputed and stored in the RWR manager. 
  Determines the best rewriting and computes the gain in the number of AIG
  nodes in the final network. In the end, p->vFanins contains information 
  about the best cut that can be used for rewriting, while p->pGraph gives 
  the decomposition dag (represented using decomposition graph data structure).
  Returns gain in the number of nodes or -1 if node cannot be rewritten.]
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
int Rwr_NodeRewrite( Rwr_Man_t * p, Cut_Man_t * pManCut, Abc_Obj_t * pNode, int fUpdateLevel, int fUseZeros, int fPlaceEnable )
{
    int fVeryVerbose = 0;
    Dec_Graph_t * pGraph;
    Cut_Cut_t * pCut;//, * pTemp;
    Abc_Obj_t * pFanin;
    unsigned uPhase;
    unsigned uTruthBest = 0; // Suppress "might be used uninitialized"
    unsigned uTruth;
    char * pPerm;
    int Required, nNodesSaved;
    int nNodesSaveCur = -1; // Suppress "might be used uninitialized"
    int i, GainCur = -1, GainBest = -1;
    abctime clk, clk2;//, Counter;

    p->nNodesConsidered++;
    // get the required times
    Required = fUpdateLevel? Abc_ObjRequiredLevel(pNode) : ABC_INFINITY;

    // get the node's cuts
clk = Abc_Clock();
    pCut = (Cut_Cut_t *)Abc_NodeGetCutsRecursive( pManCut, pNode, 0, 0 );
    assert( pCut != NULL );
p->timeCut += Abc_Clock() - clk;

//printf( " %d", Rwr_CutCountNumNodes(pNode, pCut) );
/*
    Counter = 0;
    for ( pTemp = pCut->pNext; pTemp; pTemp = pTemp->pNext )
        Counter++;
    printf( "%d ", Counter );
*/
    // go through the cuts
clk = Abc_Clock();
    for ( pCut = pCut->pNext; pCut; pCut = pCut->pNext )
    {
        // consider only 4-input cuts
        if ( pCut->nLeaves < 4 )
            continue;
//            Cut_CutPrint( pCut, 0 ), printf( "\n" );

        // get the fanin permutation
        uTruth = 0xFFFF & *Cut_CutReadTruth(pCut);
        pPerm = p->pPerms4[ (int)p->pPerms[uTruth] ];
        uPhase = p->pPhases[uTruth];
        // collect fanins with the corresponding permutation/phase
        Vec_PtrClear( p->vFaninsCur );
        Vec_PtrFill( p->vFaninsCur, (int)pCut->nLeaves, 0 );
        for ( i = 0; i < (int)pCut->nLeaves; i++ )
        {
            pFanin = Abc_NtkObj( pNode->pNtk, pCut->pLeaves[(int)pPerm[i]] );
            if ( pFanin == NULL )
                break;
            pFanin = Abc_ObjNotCond(pFanin, ((uPhase & (1<<i)) > 0) );
            Vec_PtrWriteEntry( p->vFaninsCur, i, pFanin );
        }
        if ( i != (int)pCut->nLeaves )
        {
            p->nCutsBad++;
            continue;
        }
        p->nCutsGood++;

        {
            int Counter = 0;
            Vec_PtrForEachEntry( Abc_Obj_t *, p->vFaninsCur, pFanin, i )
                if ( Abc_ObjFanoutNum(Abc_ObjRegular(pFanin)) == 1 )
                    Counter++;
            if ( Counter > 2 )
                continue;
        }

clk2 = Abc_Clock();
/*
        printf( "Considering: (" );
        Vec_PtrForEachEntry( Abc_Obj_t *, p->vFaninsCur, pFanin, i )
            printf( "%d ", Abc_ObjFanoutNum(Abc_ObjRegular(pFanin)) );
        printf( ")\n" );
*/
        // mark the fanin boundary 
        Vec_PtrForEachEntry( Abc_Obj_t *, p->vFaninsCur, pFanin, i )
            Abc_ObjRegular(pFanin)->vFanouts.nSize++;

        // label MFFC with current ID
        Abc_NtkIncrementTravId( pNode->pNtk );
        nNodesSaved = Abc_NodeMffcLabelAig( pNode );
        // unmark the fanin boundary
        Vec_PtrForEachEntry( Abc_Obj_t *, p->vFaninsCur, pFanin, i )
            Abc_ObjRegular(pFanin)->vFanouts.nSize--;
p->timeMffc += Abc_Clock() - clk2;

        // evaluate the cut
clk2 = Abc_Clock();
        pGraph = Rwr_CutEvaluate( p, pNode, pCut, p->vFaninsCur, nNodesSaved, Required, &GainCur, fPlaceEnable );
p->timeEval += Abc_Clock() - clk2;

        // check if the cut is better than the current best one
        if ( pGraph != NULL && GainBest < GainCur )
        {
            // save this form
            nNodesSaveCur = nNodesSaved;
            GainBest  = GainCur;
            p->pGraph  = pGraph;
            p->fCompl = ((uPhase & (1<<4)) > 0);
            uTruthBest = 0xFFFF & *Cut_CutReadTruth(pCut);
            // collect fanins in the
            Vec_PtrClear( p->vFanins );
            Vec_PtrForEachEntry( Abc_Obj_t *, p->vFaninsCur, pFanin, i )
                Vec_PtrPush( p->vFanins, pFanin );
        }
    }
p->timeRes += Abc_Clock() - clk;

    if ( GainBest == -1 )
        return -1;
/*
    if ( GainBest > 0 )
    {
        printf( "Class %d  ", p->pMap[uTruthBest] );
        printf( "Gain = %d. Node %d : ", GainBest, pNode->Id );
        Vec_PtrForEachEntry( Abc_Obj_t *, p->vFanins, pFanin, i )
            printf( "%d ", Abc_ObjRegular(pFanin)->Id );
        Dec_GraphPrint( stdout, p->pGraph, NULL, NULL );
        printf( "\n" );
    }
*/

//    printf( "%d", nNodesSaveCur - GainBest );
/*
    if ( GainBest > 0 )
    {
        if ( Rwr_CutIsBoolean( pNode, p->vFanins ) )
            printf( "b" );
        else
        {
            printf( "Node %d : ", pNode->Id );
            Vec_PtrForEachEntry( Abc_Obj_t *, p->vFanins, pFanin, i )
                printf( "%d ", Abc_ObjRegular(pFanin)->Id );
            printf( "a" );
        }
    }
*/
/*
    if ( GainBest > 0 )
        if ( p->fCompl )
            printf( "c" );
        else
            printf( "." );
*/

    // copy the leaves
    Vec_PtrForEachEntry( Abc_Obj_t *, p->vFanins, pFanin, i )
        Dec_GraphNode((Dec_Graph_t *)p->pGraph, i)->pFunc = pFanin;
/*
    printf( "(" );
    Vec_PtrForEachEntry( Abc_Obj_t *, p->vFanins, pFanin, i )
        printf( " %d", Abc_ObjRegular(pFanin)->vFanouts.nSize - 1 );
    printf( " )  " );
*/
//    printf( "%d ", Rwr_NodeGetDepth_rec( pNode, p->vFanins ) );

    p->nScores[p->pMap[uTruthBest]]++;
    p->nNodesGained += GainBest;
    if ( fUseZeros || GainBest > 0 )
    {
        p->nNodesRewritten++;
    }

    // report the progress
    if ( fVeryVerbose && GainBest > 0 )
    {
        printf( "Node %6s :   ", Abc_ObjName(pNode) );
        printf( "Fanins = %d. ", p->vFanins->nSize );
        printf( "Save = %d.  ", nNodesSaveCur );
        printf( "Add = %d.  ",  nNodesSaveCur-GainBest );
        printf( "GAIN = %d.  ", GainBest );
        printf( "Cone = %d.  ", p->pGraph? Dec_GraphNodeNum((Dec_Graph_t *)p->pGraph) : 0 );
        printf( "Class = %d.  ", p->pMap[uTruthBest] );
        printf( "\n" );
    }
    return GainBest;
}

/**Function*************************************************************

  Synopsis    [Profiles rewriting candidates for one node without updating the AIG.]

  Description [This is a profile-only mode for analyzing zero-gain
  rewrite candidates. It does not update the network.]
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
int Rwr_NodeRewriteProfile( Rwr_Man_t * p, Cut_Man_t * pManCut, Abc_Obj_t * pNode, int fUpdateLevel, int fUseZeros, int fPlaceEnable, Rwr_Profile_t * pProf )
{
    Rwr_ProfileRoot_t * pRoot;
    Cut_Cut_t * pCut;
    Abc_Obj_t * pFanin;
    unsigned uPhase;
    unsigned uTruth;
    char * pPerm;
    int Required, nNodesSaved;
    int i, Counter;
    abctime clk;
    (void)fUseZeros;
    (void)fPlaceEnable;

    p->nNodesConsidered++;
    pProf->nRootsConsidered++;
    pRoot = Rwr_ProfileRootAlloc( pNode->Id );
    Vec_PtrPush( pProf->vRoots, pRoot );

    Required = fUpdateLevel? Abc_ObjRequiredLevel(pNode) : ABC_INFINITY;

clk = Abc_Clock();
    pCut = (Cut_Cut_t *)Abc_NodeGetCutsRecursive( pManCut, pNode, 0, 0 );
    assert( pCut != NULL );
p->timeCut += Abc_Clock() - clk;

clk = Abc_Clock();
    for ( pCut = pCut->pNext; pCut; pCut = pCut->pNext )
    {
        if ( pCut->nLeaves < 4 )
            continue;
        pRoot->nCuts++;
        pProf->nCutsConsidered++;

        uTruth = 0xFFFF & *Cut_CutReadTruth(pCut);
        pPerm = p->pPerms4[ (int)p->pPerms[uTruth] ];
        uPhase = p->pPhases[uTruth];
        Vec_PtrClear( p->vFaninsCur );
        Vec_PtrFill( p->vFaninsCur, (int)pCut->nLeaves, 0 );
        for ( i = 0; i < (int)pCut->nLeaves; i++ )
        {
            pFanin = Abc_NtkObj( pNode->pNtk, pCut->pLeaves[(int)pPerm[i]] );
            if ( pFanin == NULL )
                break;
            pFanin = Abc_ObjNotCond(pFanin, ((uPhase & (1<<i)) > 0) );
            Vec_PtrWriteEntry( p->vFaninsCur, i, pFanin );
        }
        if ( i != (int)pCut->nLeaves )
        {
            p->nCutsBad++;
            continue;
        }
        p->nCutsGood++;

        Counter = 0;
        Vec_PtrForEachEntry( Abc_Obj_t *, p->vFaninsCur, pFanin, i )
            if ( Abc_ObjFanoutNum(Abc_ObjRegular(pFanin)) == 1 )
                Counter++;
        if ( Counter > 2 )
            continue;

        Vec_PtrForEachEntry( Abc_Obj_t *, p->vFaninsCur, pFanin, i )
            Abc_ObjRegular(pFanin)->vFanouts.nSize++;
        Abc_NtkIncrementTravId( pNode->pNtk );
        nNodesSaved = Abc_NodeMffcLabelAig( pNode );
        Vec_PtrForEachEntry( Abc_Obj_t *, p->vFaninsCur, pFanin, i )
            Abc_ObjRegular(pFanin)->vFanouts.nSize--;

        Rwr_CutProfileEvaluate( p, pNode, pCut, p->vFaninsCur, nNodesSaved, Required, pProf, pRoot );
    }
p->timeRes += Abc_Clock() - clk;

    pProf->nCandidatesConsidered += pRoot->nCandidates;
    pProf->nGainPos += pRoot->nGainPos;
    pProf->nGainZero += pRoot->nGainZero;
    pProf->nGainNeg += pRoot->nGainNeg;
    if ( pRoot->nGainPos > 0 )
        pProf->nRootsGainPos++;
    if ( pRoot->nGainZero > 0 )
        pProf->nRootsGainZero++;
    if ( pRoot->BestGain == 0 )
        pProf->nRootsBestZero++;
    return pRoot->BestGain;
}

/**Function*************************************************************

  Synopsis    [Profiles all candidates of one cut.]

  Description [This is a profile-only mode for analyzing zero-gain
  rewrite candidates. It does not update the network.]
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
static void Rwr_CutProfileEvaluate( Rwr_Man_t * p, Abc_Obj_t * pRootObj, Cut_Cut_t * pCut, Vec_Ptr_t * vFaninsCur, int nNodesSaved, int LevelMax, Rwr_Profile_t * pProf, void * pRootProf )
{
    extern int            Dec_GraphToNetworkCount( Abc_Obj_t * pRoot, Dec_Graph_t * pGraph, int NodeMax, int LevelMax );
    Rwr_ProfileRoot_t * pRoot = (Rwr_ProfileRoot_t *)pRootProf;
    Rwr_ProfileCand_t * pCand;
    Vec_Ptr_t * vSubgraphs;
    Dec_Graph_t * pGraphCur;
    Rwr_Node_t * pNode, * pFanin;
    Vec_Int_t * vAddedSigs;
    int nNodesAdded, GainCur, i, k;
    unsigned uTruth;
    uTruth = 0xFFFF & *Cut_CutReadTruth(pCut);
    vSubgraphs = Vec_VecEntry( p->vClasses, p->pMap[uTruth] );
    p->nSubgraphs += vSubgraphs->nSize;
    Vec_PtrForEachEntry( Rwr_Node_t *, vSubgraphs, pNode, i )
    {
        pGraphCur = (Dec_Graph_t *)pNode->pNext;
        Vec_PtrForEachEntry( Rwr_Node_t *, vFaninsCur, pFanin, k )
            Dec_GraphNode(pGraphCur, k)->pFunc = pFanin;
        nNodesAdded = Dec_GraphToNetworkCount( pRootObj, pGraphCur, nNodesSaved, LevelMax );
        if ( nNodesAdded == -1 )
            continue;
        assert( nNodesSaved >= nNodesAdded );
        GainCur = nNodesSaved - nNodesAdded;
        pRoot->nCandidates++;
        if ( GainCur > 0 )
            pRoot->nGainPos++;
        else if ( GainCur == 0 )
            pRoot->nGainZero++;
        else
            pRoot->nGainNeg++;
        vAddedSigs = Rwr_ProfileGraphAddedSigs( pProf, pGraphCur, vFaninsCur );
        pCand = Rwr_ProfileCandAlloc( GainCur, nNodesAdded, pGraphCur, vAddedSigs );
        if ( pRoot->BestGain < GainCur )
        {
            pRoot->BestGain = GainCur;
            pRoot->pSelected = pCand;
            pCand->fSelected = 1;
        }
        Rwr_ProfileRootAddCand( pProf, pRoot, pCand );
    }
}

/**Function*************************************************************

  Synopsis    [Evaluates the cut.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
Dec_Graph_t * Rwr_CutEvaluate( Rwr_Man_t * p, Abc_Obj_t * pRoot, Cut_Cut_t * pCut, Vec_Ptr_t * vFaninsCur, int nNodesSaved, int LevelMax, int * pGainBest, int fPlaceEnable )
{
    extern int            Dec_GraphToNetworkCount( Abc_Obj_t * pRoot, Dec_Graph_t * pGraph, int NodeMax, int LevelMax );
    Vec_Ptr_t * vSubgraphs;
    Dec_Graph_t * pGraphBest = NULL; // Suppress "might be used uninitialized"
    Dec_Graph_t * pGraphCur;
    Rwr_Node_t * pNode, * pFanin;
    int nNodesAdded, GainBest, i, k;
    unsigned uTruth;
    float CostBest;//, CostCur;
    // find the matching class of subgraphs
    uTruth = 0xFFFF & *Cut_CutReadTruth(pCut);
    vSubgraphs = Vec_VecEntry( p->vClasses, p->pMap[uTruth] );
    p->nSubgraphs += vSubgraphs->nSize;
    // determine the best subgraph
    GainBest = -1;
    CostBest = ABC_INFINITY;
    Vec_PtrForEachEntry( Rwr_Node_t *, vSubgraphs, pNode, i )
    {
        // get the current graph
        pGraphCur = (Dec_Graph_t *)pNode->pNext;
        // copy the leaves
        Vec_PtrForEachEntry( Rwr_Node_t *, vFaninsCur, pFanin, k )
            Dec_GraphNode(pGraphCur, k)->pFunc = pFanin;
        // detect how many unlabeled nodes will be reused
        nNodesAdded = Dec_GraphToNetworkCount( pRoot, pGraphCur, nNodesSaved, LevelMax );
        if ( nNodesAdded == -1 )
            continue;
        assert( nNodesSaved >= nNodesAdded );
/*
        // evaluate the cut
        if ( fPlaceEnable )
        {
            extern float Abc_PlaceEvaluateCut( Abc_Obj_t * pRoot, Vec_Ptr_t * vFanins );

            float Alpha = 0.5; // ???
            float PlaceCost;

            // get the placement cost of the cut
            PlaceCost = Abc_PlaceEvaluateCut( pRoot, vFaninsCur );

            // get the weigted cost of the cut
            CostCur = nNodesSaved - nNodesAdded + Alpha * PlaceCost;

            // do not allow uphill moves
            if ( nNodesSaved - nNodesAdded < 0 )
                continue;

            // decide what cut to use
            if ( CostBest > CostCur )
            {
                GainBest   = nNodesSaved - nNodesAdded; // pure node cost
                CostBest   = CostCur;                   // cost with placement
                pGraphBest = pGraphCur;                 // subgraph to be used for rewriting

                // score the graph
                if ( nNodesSaved - nNodesAdded > 0 )
                {
                    pNode->nScore++;
                    pNode->nGain += GainBest;
                    pNode->nAdded += nNodesAdded;
                }
            }
        }
        else
*/
        {
            // count the gain at this node
            if ( GainBest < nNodesSaved - nNodesAdded )
            {
                GainBest   = nNodesSaved - nNodesAdded;
                pGraphBest = pGraphCur;

                // score the graph
                if ( nNodesSaved - nNodesAdded > 0 )
                {
                    pNode->nScore++;
                    pNode->nGain += GainBest;
                    pNode->nAdded += nNodesAdded;
                }
            }
        }
    }
    if ( GainBest == -1 )
        return NULL;
    *pGainBest = GainBest;
    return pGraphBest;
}

/**Function*************************************************************

  Synopsis    [Checks the type of the cut.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Rwr_CutIsBoolean_rec( Abc_Obj_t * pObj, Vec_Ptr_t * vLeaves, int fMarkA )
{
    if ( Vec_PtrFind(vLeaves, pObj) >= 0 || Vec_PtrFind(vLeaves, Abc_ObjNot(pObj)) >= 0 )
    {
        if ( fMarkA )
            pObj->fMarkA = 1;
        else
            pObj->fMarkB = 1;
        return;
    }
    assert( !Abc_ObjIsCi(pObj) );
    Rwr_CutIsBoolean_rec( Abc_ObjFanin0(pObj), vLeaves, fMarkA );
    Rwr_CutIsBoolean_rec( Abc_ObjFanin1(pObj), vLeaves, fMarkA );
}

/**Function*************************************************************

  Synopsis    [Checks the type of the cut.]

  Description [Returns 1(0) if the cut is Boolean (algebraic).]
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
int Rwr_CutIsBoolean( Abc_Obj_t * pObj, Vec_Ptr_t * vLeaves )
{
    Abc_Obj_t * pTemp;
    int i, RetValue;
    Vec_PtrForEachEntry( Abc_Obj_t *, vLeaves, pTemp, i )
    {
        pTemp = Abc_ObjRegular(pTemp);
        assert( !pTemp->fMarkA && !pTemp->fMarkB );
    }
    Rwr_CutIsBoolean_rec( Abc_ObjFanin0(pObj), vLeaves, 1 );
    Rwr_CutIsBoolean_rec( Abc_ObjFanin1(pObj), vLeaves, 0 );
    RetValue = 0;
    Vec_PtrForEachEntry( Abc_Obj_t *, vLeaves, pTemp, i )
    {
        pTemp = Abc_ObjRegular(pTemp);
        RetValue |= pTemp->fMarkA && pTemp->fMarkB;
        pTemp->fMarkA = pTemp->fMarkB = 0;
    }
    return RetValue;
}


/**Function*************************************************************

  Synopsis    [Count the nodes in the cut space of a node.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Rwr_CutCountNumNodes_rec( Abc_Obj_t * pObj, Cut_Cut_t * pCut, Vec_Ptr_t * vNodes )
{
    int i;
    for ( i = 0; i < (int)pCut->nLeaves; i++ )
        if ( pCut->pLeaves[i] == pObj->Id )
        {
            // check if the node is collected
            if ( pObj->fMarkC == 0 )
            {
                pObj->fMarkC = 1;
                Vec_PtrPush( vNodes, pObj );
            }
            return;
        }
    assert( Abc_ObjIsNode(pObj) );
    // check if the node is collected
    if ( pObj->fMarkC == 0 )
    {
        pObj->fMarkC = 1;
        Vec_PtrPush( vNodes, pObj );
    }
    // traverse the fanins
    Rwr_CutCountNumNodes_rec( Abc_ObjFanin0(pObj), pCut, vNodes );
    Rwr_CutCountNumNodes_rec( Abc_ObjFanin1(pObj), pCut, vNodes );
}

/**Function*************************************************************

  Synopsis    [Count the nodes in the cut space of a node.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
int Rwr_CutCountNumNodes( Abc_Obj_t * pObj, Cut_Cut_t * pCut )
{
    Vec_Ptr_t * vNodes;
    int i, Counter;
    // collect all nodes
    vNodes = Vec_PtrAlloc( 100 );
    for ( pCut = pCut->pNext; pCut; pCut = pCut->pNext )
        Rwr_CutCountNumNodes_rec( pObj, pCut, vNodes );
    // clean all nodes
    Vec_PtrForEachEntry( Abc_Obj_t *, vNodes, pObj, i )
        pObj->fMarkC = 0;
    // delete and return
    Counter = Vec_PtrSize(vNodes);
    Vec_PtrFree( vNodes );
    return Counter;
}


/**Function*************************************************************

  Synopsis    [Returns depth of the cut.]

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
int Rwr_NodeGetDepth_rec( Abc_Obj_t * pObj, Vec_Ptr_t * vLeaves )
{
    Abc_Obj_t * pLeaf;
    int i, Depth0, Depth1;
    if ( Abc_ObjIsCi(pObj) )
        return 0;
    Vec_PtrForEachEntry( Abc_Obj_t *, vLeaves, pLeaf, i )
        if ( pObj == Abc_ObjRegular(pLeaf) )
            return 0;
    Depth0 = Rwr_NodeGetDepth_rec( Abc_ObjFanin0(pObj), vLeaves );
    Depth1 = Rwr_NodeGetDepth_rec( Abc_ObjFanin1(pObj), vLeaves );
    return 1 + Abc_MaxInt( Depth0, Depth1 );
}


/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Rwr_ScoresClean( Rwr_Man_t * p )
{
    Vec_Ptr_t * vSubgraphs;
    Rwr_Node_t * pNode;
    int i, k;
    for ( i = 0; i < p->vClasses->nSize; i++ )
    {
        vSubgraphs = Vec_VecEntry( p->vClasses, i );
        Vec_PtrForEachEntry( Rwr_Node_t *, vSubgraphs, pNode, k )
            pNode->nScore = pNode->nGain = pNode->nAdded = 0;
    }
}

static int Gains[222];

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
int Rwr_ScoresCompare( int * pNum1, int * pNum2 )
{
    if ( Gains[*pNum1] > Gains[*pNum2] )
        return -1;
    if ( Gains[*pNum1] < Gains[*pNum2] )
        return 1;
    return 0;
}

/**Function*************************************************************

  Synopsis    []

  Description []
               
  SideEffects []

  SeeAlso     []

***********************************************************************/
void Rwr_ScoresReport( Rwr_Man_t * p )
{
    extern void Ivy_TruthDsdComputePrint( unsigned uTruth );
    int Perm[222];
    Vec_Ptr_t * vSubgraphs;
    Rwr_Node_t * pNode;
    int i, iNew, k;
    unsigned uTruth;
    // collect total gains
    assert( p->vClasses->nSize == 222 );
    for ( i = 0; i < p->vClasses->nSize; i++ )
    {
        Perm[i] = i;
        Gains[i] = 0;
        vSubgraphs = Vec_VecEntry( p->vClasses, i );
        Vec_PtrForEachEntry( Rwr_Node_t *, vSubgraphs, pNode, k )
            Gains[i] += pNode->nGain;
    }
    // sort the gains
    qsort( Perm, (size_t)222, sizeof(int), (int (*)(const void *, const void *))Rwr_ScoresCompare );

    // print classes
    for ( i = 0; i < p->vClasses->nSize; i++ )
    {
        iNew = Perm[i];
        if ( Gains[iNew] == 0 )
            break;
        vSubgraphs = Vec_VecEntry( p->vClasses, iNew );
        printf( "CLASS %3d: Subgr = %3d. Total gain = %6d.  ", iNew, Vec_PtrSize(vSubgraphs), Gains[iNew] );
        uTruth = (unsigned)p->pMapInv[iNew];
        Extra_PrintBinary( stdout, &uTruth, 16 );
        printf( "  " );
        Ivy_TruthDsdComputePrint( (unsigned)p->pMapInv[iNew] | ((unsigned)p->pMapInv[iNew] << 16) );
        Vec_PtrForEachEntry( Rwr_Node_t *, vSubgraphs, pNode, k )
        {
            if ( pNode->nScore == 0 )
                continue;
            printf( "    %2d: S=%5d. A=%5d. G=%6d. ", k, pNode->nScore, pNode->nAdded, pNode->nGain );
            Dec_GraphPrint( stdout, (Dec_Graph_t *)pNode->pNext, NULL, NULL );
        }
    }
}

////////////////////////////////////////////////////////////////////////
///                       END OF FILE                                ///
////////////////////////////////////////////////////////////////////////


ABC_NAMESPACE_IMPL_END
