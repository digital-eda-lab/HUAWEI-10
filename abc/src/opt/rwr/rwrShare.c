/**CFile****************************************************************

  FileName    [rwrShare.c]

  SystemName  [ABC: Logic synthesis and verification system.]

  PackageName [DAG-aware AIG rewriting package.]

  Synopsis    [Shared-aware zero-cost rewriting.]

***********************************************************************/

#include "opt/rwr/rwrShare.h"

ABC_NAMESPACE_IMPL_START

typedef struct Rwr_ShareBest_t_ Rwr_ShareBest_t;

struct Rwr_ShareBest_t_
{
    int          fSet;
    int          Gain;
    int          Share;
    int          ExistingReuse;
    int          nNodesSaved;
    int          nNodesAdded;
    int          fCompl;
    unsigned     uTruth;
    long long    CandId;
    Dec_Graph_t * pGraph;
};

static int Rwr_ShareCandIsBetter( int Gain, int Share, int ExistingReuse, Rwr_ShareBest_t * pBest, int fUseEqualGainShare )
{
    if ( !pBest->fSet )
        return 1;
    if ( Gain != pBest->Gain )
        return Gain > pBest->Gain;
    if ( Gain == 0 || fUseEqualGainShare )
    {
        if ( Share != pBest->Share )
            return Share > pBest->Share;
        if ( ExistingReuse != pBest->ExistingReuse )
            return ExistingReuse > pBest->ExistingReuse;
    }
    return 0;
}

static void Rwr_ShareBestUpdate( Rwr_ShareBest_t * pBest, Dec_Graph_t * pGraph, int Gain, int Share, int ExistingReuse, int nNodesSaved, int nNodesAdded, int fCompl, unsigned uTruth, long long CandId )
{
    pBest->fSet = 1;
    pBest->Gain = Gain;
    pBest->Share = Share;
    pBest->ExistingReuse = ExistingReuse;
    pBest->nNodesSaved = nNodesSaved;
    pBest->nNodesAdded = nNodesAdded;
    pBest->fCompl = fCompl;
    pBest->uTruth = uTruth;
    pBest->CandId = CandId;
    pBest->pGraph = pGraph;
}

static void Rwr_ShareFaninsCopy( Vec_Ptr_t * vFaninsTo, Vec_Ptr_t * vFaninsFrom )
{
    Abc_Obj_t * pFanin;
    int i;
    Vec_PtrClear( vFaninsTo );
    Vec_PtrForEachEntry( Abc_Obj_t *, vFaninsFrom, pFanin, i )
        Vec_PtrPush( vFaninsTo, pFanin );
}

static const char * Rwr_ShareScoreModeName( int nScoreMode )
{
    if ( nScoreMode == 2 )
        return "root frequency";
    if ( nScoreMode == 3 )
        return "normalized root frequency + existing reuse";
    return "raw frequency";
}

static void Rwr_ShareCollectMffcMarked_rec( Abc_Obj_t * pObj, Vec_Ptr_t * vMffc, Vec_Int_t * vSeen )
{
    Abc_Obj_t * pFanin;
    int i, ObjId;

    if ( pObj == NULL )
        return;
    if ( !Abc_ObjIsNode(pObj) )
        return;
    if ( !Abc_NodeIsTravIdCurrent(pObj) )
        return;

    ObjId = Abc_ObjId(pObj);
    if ( Vec_IntSize(vSeen) <= ObjId )
        Vec_IntFillExtra( vSeen, ObjId + 1, 0 );
    if ( Vec_IntEntry(vSeen, ObjId) )
        return;

    Vec_IntWriteEntry( vSeen, ObjId, 1 );
    Vec_PtrPush( vMffc, pObj );

    Abc_ObjForEachFanin( pObj, pFanin, i )
        Rwr_ShareCollectMffcMarked_rec( pFanin, vMffc, vSeen );
}

static int Rwr_ShareFaninsFromIds( Abc_Ntk_t * pNtk, Vec_Int_t * vFaninIds, Vec_Ptr_t * vFanins )
{
    Abc_Obj_t * pFanin;
    int i, ObjId;
    Vec_PtrClear( vFanins );
    Vec_IntForEachEntry( vFaninIds, ObjId, i )
    {
        pFanin = Abc_NtkObj( pNtk, ObjId );
        if ( pFanin == NULL )
            return 0;
        Vec_PtrPush( vFanins, pFanin );
    }
    return 1;
}

void Rwr_ShareProbeDestroyedMffc( Rwr_Profile_t * pProf, Abc_Obj_t * pNode, Vec_Int_t * vFaninIds, int nGain, int nSelectedShare, Rwr_ShareStats_t * pStats, int nDestroyProbeThreshold, int fVerbose )
{
    Vec_Ptr_t * vFanins, * vMffc;
    Vec_Int_t * vSeen;
    Abc_Obj_t * pObj;
    int i, SigLit, Freq, RootFreq, MaxFreq = 0, MaxRootFreq = 0, CrossRootSigs = 0, fHighDestroy;
    long long DestroyRaw = 0, DestroyRoot = 0;

    if ( pProf == NULL || pNode == NULL || vFaninIds == NULL || pStats == NULL || nDestroyProbeThreshold < 0 )
        return;

    vFanins = Vec_PtrAlloc( Vec_IntSize(vFaninIds) );
    vMffc = Vec_PtrAlloc( 16 );
    vSeen = Vec_IntAlloc( 16 );

    if ( Rwr_ShareFaninsFromIds( pNode->pNtk, vFaninIds, vFanins ) )
    {
        Vec_PtrForEachEntry( Abc_Obj_t *, vFanins, pObj, i )
            Abc_ObjRegular(pObj)->vFanouts.nSize++;

        Abc_NtkIncrementTravId( pNode->pNtk );
        Abc_NodeMffcLabelAig( pNode );
        Rwr_ShareCollectMffcMarked_rec( pNode, vMffc, vSeen );

        Vec_PtrForEachEntry( Abc_Obj_t *, vFanins, pObj, i )
            Abc_ObjRegular(pObj)->vFanouts.nSize--;
    }

    /*
     * Main destroyed-share score is occurrence based: repeated signatures in the
     * same deleted MFFC contribute once per deleted node and are not deduplicated.
     */
    Vec_PtrForEachEntry( Abc_Obj_t *, vMffc, pObj, i )
    {
        SigLit = Rwr_ProfileObjSigLit( pProf, pObj );
        if ( SigLit < 0 )
            continue;
        Freq = Rwr_ProfileSigFreqByLit( pProf, SigLit );
        RootFreq = Rwr_ProfileSigRootFreqByLit( pProf, SigLit );
        if ( Freq > 1 )
            DestroyRaw += Freq - 1;
        if ( RootFreq > 1 )
        {
            DestroyRoot += RootFreq - 1;
            CrossRootSigs++;
        }
        if ( MaxFreq < Freq )
            MaxFreq = Freq;
        if ( MaxRootFreq < RootFreq )
            MaxRootFreq = RootFreq;
    }

    pStats->nDestroyChecked++;
    pStats->nDestroyDeletedNodes += Vec_PtrSize(vMffc);
    pStats->nDestroyRawTotal += DestroyRaw;
    pStats->nDestroyRootTotal += DestroyRoot;
    pStats->nDestroyCrossRootSigsTotal += CrossRootSigs;
    if ( pStats->nDestroyMaxRaw < DestroyRaw )
        pStats->nDestroyMaxRaw = DestroyRaw;
    if ( pStats->nDestroyMaxRoot < DestroyRoot )
        pStats->nDestroyMaxRoot = DestroyRoot;

    if ( nGain > 0 )
    {
        pStats->nDestroyPosChecked++;
        pStats->nDestroyPosRawTotal += DestroyRaw;
        pStats->nDestroyPosRootTotal += DestroyRoot;
    }
    else
    {
        pStats->nDestroyZeroChecked++;
        pStats->nDestroyZeroRawTotal += DestroyRaw;
        pStats->nDestroyZeroRootTotal += DestroyRoot;
    }

    fHighDestroy = DestroyRaw >= nDestroyProbeThreshold || DestroyRoot >= nDestroyProbeThreshold;
    if ( fHighDestroy )
    {
        pStats->nDestroyHighRoots++;
        if ( nGain > 0 )
            pStats->nDestroyHighPosRoots++;
        else
            pStats->nDestroyHighZeroRoots++;
        if ( nGain == 1 )
            pStats->nDestroyGain1HighRoots++;
        else if ( nGain >= 2 )
            pStats->nDestroyGain2PlusHighRoots++;
        if ( fVerbose )
            printf( "DESTROY root=%d gain=%d added_share=%d destroy_raw=%lld destroy_root=%lld mffc=%d cross_sigs=%d max_freq=%d max_root_freq=%d\n",
                Abc_ObjId(pNode), nGain, nSelectedShare, DestroyRaw, DestroyRoot, Vec_PtrSize(vMffc), CrossRootSigs, MaxFreq, MaxRootFreq );
    }

    Vec_IntFree( vSeen );
    Vec_PtrFree( vMffc );
    Vec_PtrFree( vFanins );
}

static void Rwr_CutEvaluateShare( Rwr_Man_t * p, Abc_Obj_t * pRoot, Cut_Cut_t * pCut, Vec_Ptr_t * vFaninsCur, Vec_Ptr_t * vFaninsLocal, Vec_Ptr_t * vFaninsShare, int nNodesSaved, int LevelMax, int fCompl, unsigned uTruthCut, Rwr_Profile_t * pProf, Rwr_ShareStats_t * pStats, Rwr_ShareBest_t * pBestShare, Rwr_ShareBest_t * pBestLocal, long long * pCandId, int nScoreMode, int fUseEqualGainShare )
{
    extern int Dec_GraphToNetworkCount( Abc_Obj_t * pRoot, Dec_Graph_t * pGraph, int NodeMax, int LevelMax );
    Vec_Ptr_t * vSubgraphs;
    Dec_Graph_t * pGraphCur;
    Rwr_Node_t * pNode, * pFanin;
    Vec_Int_t * vAddedSigs;
    int i, k, nNodesAdded, GainCur, ShareCur, ExistingReuse;

    vSubgraphs = Vec_VecEntry( p->vClasses, p->pMap[uTruthCut] );
    p->nSubgraphs += vSubgraphs->nSize;
    Vec_PtrForEachEntry( Rwr_Node_t *, vSubgraphs, pNode, i )
    {
        pGraphCur = (Dec_Graph_t *)pNode->pNext;
        Vec_PtrForEachEntry( Rwr_Node_t *, vFaninsCur, pFanin, k )
            Dec_GraphNode(pGraphCur, k)->pFunc = pFanin;
        nNodesAdded = Dec_GraphToNetworkCount( pRoot, pGraphCur, nNodesSaved, LevelMax );
        if ( nNodesAdded == -1 )
            continue;

        GainCur = nNodesSaved - nNodesAdded;
        pStats->nCandidatesConsidered++;
        if ( GainCur > 0 )
            pStats->nGainPos++;
        else if ( GainCur == 0 )
            pStats->nGainZero++;
        else
        {
            pStats->nGainNeg++;
            continue;
        }

        (*pCandId)++;
        ExistingReuse = Dec_GraphNodeNum(pGraphCur) - nNodesAdded;
        vAddedSigs = Rwr_ProfileGraphAddedSigs( pProf, pGraphCur, vFaninsCur );
        ShareCur = Rwr_ProfileScoreAddedSigsMode( pProf, vAddedSigs, ExistingReuse, nScoreMode );
        Vec_IntFree( vAddedSigs );

        if ( !pBestLocal->fSet || pBestLocal->Gain < GainCur )
        {
            Rwr_ShareBestUpdate( pBestLocal, pGraphCur, GainCur, ShareCur, ExistingReuse, nNodesSaved, nNodesAdded, fCompl, uTruthCut, *pCandId );
            Rwr_ShareFaninsCopy( vFaninsLocal, vFaninsCur );
        }
        if ( Rwr_ShareCandIsBetter( GainCur, ShareCur, ExistingReuse, pBestShare, fUseEqualGainShare ) )
        {
            Rwr_ShareBestUpdate( pBestShare, pGraphCur, GainCur, ShareCur, ExistingReuse, nNodesSaved, nNodesAdded, fCompl, uTruthCut, *pCandId );
            Rwr_ShareFaninsCopy( vFaninsShare, vFaninsCur );
        }
    }
}

int Rwr_NodeRewriteShare( Rwr_Man_t * p, Cut_Man_t * pManCut, Abc_Obj_t * pNode, int fUpdateLevel, int fUseZeros, int fPlaceEnable, Rwr_Profile_t * pProf, Rwr_ShareStats_t * pStats, int fUseShareSelect, int nShareDeltaMin, int nScoreMode, int fUseEqualGainShare, Vec_Int_t * vSelectedFaninIds, int * pSelectedShare )
{
    Rwr_ShareBest_t BestShare, BestLocal, * pBestActual;
    Vec_Ptr_t * vFaninsLocal, * vFaninsShare, * vFaninsActual;
    Cut_Cut_t * pCut;
    Abc_Obj_t * pFanin;
    unsigned uPhase, uTruth;
    char * pPerm;
    int Required, nNodesSaved, i, Counter;
    long long CandId = 0;
    abctime clk, clk2;
    (void)fPlaceEnable;

    memset( &BestShare, 0, sizeof(Rwr_ShareBest_t) );
    memset( &BestLocal, 0, sizeof(Rwr_ShareBest_t) );
    vFaninsLocal = Vec_PtrAlloc( 4 );
    vFaninsShare = Vec_PtrAlloc( 4 );
    if ( vSelectedFaninIds )
        Vec_IntClear( vSelectedFaninIds );
    if ( pSelectedShare )
        *pSelectedShare = -1;

    p->nNodesConsidered++;
    pStats->nRootsConsidered++;
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

clk2 = Abc_Clock();
        Vec_PtrForEachEntry( Abc_Obj_t *, p->vFaninsCur, pFanin, i )
            Abc_ObjRegular(pFanin)->vFanouts.nSize++;
        Abc_NtkIncrementTravId( pNode->pNtk );
        nNodesSaved = Abc_NodeMffcLabelAig( pNode );
        Vec_PtrForEachEntry( Abc_Obj_t *, p->vFaninsCur, pFanin, i )
            Abc_ObjRegular(pFanin)->vFanouts.nSize--;
p->timeMffc += Abc_Clock() - clk2;

clk2 = Abc_Clock();
        Rwr_CutEvaluateShare( p, pNode, pCut, p->vFaninsCur, vFaninsLocal, vFaninsShare, nNodesSaved, Required, ((uPhase & (1<<4)) > 0), uTruth, pProf, pStats, &BestShare, &BestLocal, &CandId, nScoreMode, fUseEqualGainShare );
p->timeEval += Abc_Clock() - clk2;
    }
p->timeRes += Abc_Clock() - clk;

    if ( !BestLocal.fSet )
    {
        Vec_PtrFree( vFaninsLocal );
        Vec_PtrFree( vFaninsShare );
        return -1;
    }

    pBestActual = &BestLocal;
    vFaninsActual = vFaninsLocal;
    if ( BestLocal.fSet && BestLocal.Gain == 0 )
    {
        int fWouldChange, fActuallyChanged, fBlockedByThreshold, ShareDelta;
        pStats->nZeroRootsAnalyzed++;
        pStats->nOriginalShareSum += BestLocal.Share;
        pStats->nBestShareSum += BestShare.Share;
        ShareDelta = BestShare.Share - BestLocal.Share;
        if ( pStats->nMaxShareDelta < ShareDelta )
            pStats->nMaxShareDelta = ShareDelta;
        fWouldChange = BestShare.fSet && BestShare.Gain == 0 && BestShare.CandId != BestLocal.CandId;
        fBlockedByThreshold = fWouldChange && ShareDelta < nShareDeltaMin;
        if ( fWouldChange )
        {
            pStats->nWouldChangeRoots++;
            pStats->nWouldChangeDeltaSum += ShareDelta;
        }
        if ( fUseShareSelect && fWouldChange && ShareDelta >= nShareDeltaMin )
        {
            pBestActual = &BestShare;
            vFaninsActual = vFaninsShare;
        }
        fActuallyChanged = pBestActual->CandId != BestLocal.CandId;
        if ( fBlockedByThreshold )
            pStats->nBlockedByThreshold++;
        if ( fActuallyChanged )
        {
            pStats->nActuallyChangedRoots++;
            pStats->nActualChangeDeltaSum += ShareDelta;
        }
        pStats->nActualShareSum += pBestActual->Share;
        pStats->nShareImprovementSum += pBestActual->Share - BestLocal.Share;
    }
    else if ( fUseEqualGainShare && BestLocal.fSet && BestLocal.Gain > 0 )
    {
        int fWouldChange, fActuallyChanged, fBlockedByThreshold, ShareDelta;
        pStats->nPosRootsAnalyzed++;
        ShareDelta = BestShare.Share - BestLocal.Share;
        fWouldChange = BestShare.fSet && BestShare.Gain == BestLocal.Gain && BestShare.CandId != BestLocal.CandId;
        fBlockedByThreshold = fWouldChange && ShareDelta < nShareDeltaMin;
        if ( fWouldChange )
        {
            pStats->nPosWouldChangeRoots++;
            pStats->nPosWouldChangeDeltaSum += ShareDelta;
        }
        if ( fUseShareSelect && fWouldChange && ShareDelta >= nShareDeltaMin )
        {
            pBestActual = &BestShare;
            vFaninsActual = vFaninsShare;
        }
        fActuallyChanged = pBestActual->CandId != BestLocal.CandId;
        if ( fBlockedByThreshold )
            pStats->nPosBlockedByThreshold++;
        if ( fActuallyChanged )
        {
            pStats->nPosActuallyChangedRoots++;
            pStats->nPosActualChangeDeltaSum += ShareDelta;
        }
        pStats->nPosShareImprovementSum += pBestActual->Share - BestLocal.Share;
    }

    Rwr_ShareFaninsCopy( p->vFanins, vFaninsActual );
    if ( vSelectedFaninIds )
    {
        Vec_IntClear( vSelectedFaninIds );
        Vec_PtrForEachEntry( Abc_Obj_t *, vFaninsActual, pFanin, i )
            Vec_IntPush( vSelectedFaninIds, Abc_ObjId(Abc_ObjRegular(pFanin)) );
    }
    if ( pSelectedShare )
        *pSelectedShare = pBestActual->Share;
    p->pGraph = pBestActual->pGraph;
    p->fCompl = pBestActual->fCompl;
    Vec_PtrForEachEntry( Abc_Obj_t *, p->vFanins, pFanin, i )
        Dec_GraphNode((Dec_Graph_t *)p->pGraph, i)->pFunc = pFanin;

    p->nScores[p->pMap[pBestActual->uTruth]]++;
    p->nNodesGained += pBestActual->Gain;
    if ( fUseZeros || pBestActual->Gain > 0 )
        p->nNodesRewritten++;

    Vec_PtrFree( vFaninsLocal );
    Vec_PtrFree( vFaninsShare );
    return pBestActual->Gain;
}

void Rwr_ShareStatsPrint( Rwr_ShareStats_t * pStats )
{
    double DenZero = pStats->nZeroRootsAnalyzed ? (double)pStats->nZeroRootsAnalyzed : 1.0;
    double DenWould = pStats->nWouldChangeRoots ? (double)pStats->nWouldChangeRoots : 1.0;
    double DenActual = pStats->nActuallyChangedRoots ? (double)pStats->nActuallyChangedRoots : 1.0;
    double DenPosWould = pStats->nPosWouldChangeRoots ? (double)pStats->nPosWouldChangeRoots : 1.0;
    double DenPosActual = pStats->nPosActuallyChangedRoots ? (double)pStats->nPosActuallyChangedRoots : 1.0;
    double DenBlocks = pStats->nProfileBuilds ? (double)pStats->nProfileBuilds : 1.0;
    double DenEffBlocks = pStats->nProfileBlocksEffective ? (double)pStats->nProfileBlocksEffective : 1.0;
    double DenDestroy = pStats->nDestroyChecked ? (double)pStats->nDestroyChecked : 1.0;
    double DenDestroyPos = pStats->nDestroyPosChecked ? (double)pStats->nDestroyPosChecked : 1.0;
    double DenDestroyZero = pStats->nDestroyZeroChecked ? (double)pStats->nDestroyZeroChecked : 1.0;
    double TimeProfile = 1.0 * (double)pStats->timeProfileTotal / (double)CLOCKS_PER_SEC;
    double TimeRewrite = 1.0 * (double)pStats->timeRewriteTotal / (double)CLOCKS_PER_SEC;
    printf( "Rewrite-share summary:\n" );
    printf( "  Share selection enabled          : %d\n", pStats->fUseShareSelect );
    printf( "  Equal-gain sharing enabled       : %d\n", pStats->fUseEqualGainShare );
    printf( "  Share delta threshold            : %d\n", pStats->nShareDeltaMin );
    printf( "  Score mode                       : %d (%s)\n", pStats->nScoreMode, Rwr_ShareScoreModeName(pStats->nScoreMode) );
    printf( "  Profile blocks requested         : %d\n", pStats->nProfileBlocksRequested );
    printf( "  Profile blocks effective         : %d\n", pStats->nProfileBlocksEffective );
    printf( "  Profile rebuilds                 : %d\n", pStats->nProfileBuilds );
    printf( "  Total profile time               : %.6f\n", TimeProfile );
    printf( "  Total rewrite time               : %.6f\n", TimeRewrite );
    printf( "  Average profile time per block   : %.6f\n", TimeProfile / DenBlocks );
    printf( "  Roots scheduled                  : %lld\n", pStats->nRootsScheduled );
    printf( "  Roots processed                  : %lld\n", pStats->nRootsProcessed );
    printf( "  Roots skipped                    : %lld\n", pStats->nRootsSkipped );
    printf( "  Average roots per block          : %.2f\n", pStats->nRootsScheduled / DenEffBlocks );
    printf( "\n" );
    printf( "  Roots considered                  : %lld\n", pStats->nRootsConsidered );
    printf( "  Candidates considered             : %lld\n", pStats->nCandidatesConsidered );
    printf( "  Gain > 0 candidates               : %lld\n", pStats->nGainPos );
    printf( "  Gain = 0 candidates               : %lld\n", pStats->nGainZero );
    printf( "  Gain < 0 candidates               : %lld\n", pStats->nGainNeg );
    printf( "\n" );
    printf( "  Positive-gain rewrites            : %lld\n", pStats->nRewritePos );
    printf( "  Zero-gain rewrites                : %lld\n", pStats->nRewriteZero );
    printf( "\n" );
    printf( "  Zero-gain roots analyzed          : %lld\n", pStats->nZeroRootsAnalyzed );
    printf( "  Zero-gain roots where share would change selection : %lld\n", pStats->nWouldChangeRoots );
    printf( "  Zero-gain roots blocked by threshold: %lld\n", pStats->nBlockedByThreshold );
    printf( "  Zero-gain roots actually changed selection : %lld\n", pStats->nActuallyChangedRoots );
    printf( "\n" );
    printf( "  Avg original/best-local sharing score : %.2f\n", pStats->nOriginalShareSum / DenZero );
    printf( "  Avg best-share sharing score          : %.2f\n", pStats->nBestShareSum / DenZero );
    printf( "  Avg actual selected sharing score     : %.2f\n", pStats->nActualShareSum / DenZero );
    printf( "\n" );
    printf( "  Avg share delta among would-change roots : %.2f\n", pStats->nWouldChangeDeltaSum / DenWould );
    printf( "  Avg share delta among actual changed roots: %.2f\n", pStats->nActualChangeDeltaSum / DenActual );
    printf( "  Max share delta                         : %lld\n", pStats->nMaxShareDelta );
    printf( "  Estimated sharing score improvement     : %.2f\n", pStats->nShareImprovementSum / DenZero );
    printf( "\n" );
    printf( "  Positive-gain roots analyzed for equal-share tie-break : %lld\n", pStats->nPosRootsAnalyzed );
    printf( "  Positive-gain roots where share would change selection : %lld\n", pStats->nPosWouldChangeRoots );
    printf( "  Positive-gain roots blocked by threshold: %lld\n", pStats->nPosBlockedByThreshold );
    printf( "  Positive-gain roots actually changed selection : %lld\n", pStats->nPosActuallyChangedRoots );
    printf( "  Avg positive share delta among would-change roots : %.2f\n", pStats->nPosWouldChangeDeltaSum / DenPosWould );
    printf( "  Avg positive share delta among actual changed roots: %.2f\n", pStats->nPosActualChangeDeltaSum / DenPosActual );
    printf( "  Estimated positive sharing score improvement     : %.2f\n", pStats->nPosRootsAnalyzed ? pStats->nPosShareImprovementSum / (double)pStats->nPosRootsAnalyzed : 0.0 );
    printf( "  Equal-gain roots actually changed selection      : %lld\n", pStats->nActuallyChangedRoots + pStats->nPosActuallyChangedRoots );
    printf( "\n" );
    printf( "  Profile repeated signatures       : %lld\n", pStats->nProfileRepeatedSigsLast );
    printf( "  Profile cross-root signatures     : %lld\n", pStats->nProfileCrossRootSigsLast );
    printf( "  Profile repeated signatures sum   : %lld\n", pStats->nProfileRepeatedSigsSum );
    printf( "  Profile cross-root signatures sum : %lld\n", pStats->nProfileCrossRootSigsSum );
    if ( pStats->fDestroyProbe )
    {
        printf( "\n" );
        printf( "Destroy-share probe:\n" );
        printf( "  Enabled                              : %d\n", pStats->fDestroyProbe );
        printf( "  High destroy threshold               : %d\n", pStats->nDestroyProbeThreshold );
        printf( "  Rewrites checked                     : %lld\n", pStats->nDestroyChecked );
        printf( "  Deleted MFFC nodes total             : %lld\n", pStats->nDestroyDeletedNodes );
        printf( "  Avg deleted MFFC nodes per rewrite   : %.2f\n", pStats->nDestroyDeletedNodes / DenDestroy );
        printf( "\n" );
        printf( "  Destroy raw score total              : %lld\n", pStats->nDestroyRawTotal );
        printf( "  Destroy root score total             : %lld\n", pStats->nDestroyRootTotal );
        printf( "  Deleted cross-root sig occurrences   : %lld\n", pStats->nDestroyCrossRootSigsTotal );
        printf( "  Avg destroy raw per rewrite          : %.2f\n", pStats->nDestroyRawTotal / DenDestroy );
        printf( "  Avg destroy root per rewrite         : %.2f\n", pStats->nDestroyRootTotal / DenDestroy );
        printf( "  Max destroy raw score                : %lld\n", pStats->nDestroyMaxRaw );
        printf( "  Max destroy root score               : %lld\n", pStats->nDestroyMaxRoot );
        printf( "\n" );
        printf( "  Positive-gain rewrites checked       : %lld\n", pStats->nDestroyPosChecked );
        printf( "  Positive-gain destroy raw total      : %lld\n", pStats->nDestroyPosRawTotal );
        printf( "  Positive-gain destroy root total     : %lld\n", pStats->nDestroyPosRootTotal );
        printf( "  Avg positive destroy raw             : %.2f\n", pStats->nDestroyPosRawTotal / DenDestroyPos );
        printf( "  Avg positive destroy root            : %.2f\n", pStats->nDestroyPosRootTotal / DenDestroyPos );
        printf( "\n" );
        printf( "  Zero-gain rewrites checked           : %lld\n", pStats->nDestroyZeroChecked );
        printf( "  Zero-gain destroy raw total          : %lld\n", pStats->nDestroyZeroRawTotal );
        printf( "  Zero-gain destroy root total         : %lld\n", pStats->nDestroyZeroRootTotal );
        printf( "  Avg zero destroy raw                 : %.2f\n", pStats->nDestroyZeroRawTotal / DenDestroyZero );
        printf( "  Avg zero destroy root                : %.2f\n", pStats->nDestroyZeroRootTotal / DenDestroyZero );
        printf( "\n" );
        printf( "  High-destroy rewrites                : %lld\n", pStats->nDestroyHighRoots );
        printf( "  High-destroy positive rewrites       : %lld\n", pStats->nDestroyHighPosRoots );
        printf( "  High-destroy zero rewrites           : %lld\n", pStats->nDestroyHighZeroRoots );
        printf( "  High-destroy gain=1 rewrites         : %lld\n", pStats->nDestroyGain1HighRoots );
        printf( "  High-destroy gain>=2 rewrites        : %lld\n", pStats->nDestroyGain2PlusHighRoots );
    }
}

ABC_NAMESPACE_IMPL_END

////////////////////////////////////////////////////////////////////////
///                       END OF FILE                                ///
////////////////////////////////////////////////////////////////////////
