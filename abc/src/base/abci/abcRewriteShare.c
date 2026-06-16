/**CFile****************************************************************

  FileName    [abcRewriteShare.c]

  SystemName  [ABC: Logic synthesis and verification system.]

  PackageName [Network and node package.]

  Synopsis    [Shared-aware zero-cost AIG rewriting command.]

***********************************************************************/

#include "base/abc/abc.h"
#include "opt/rwr/rwrShare.h"
#include "bool/dec/dec.h"

ABC_NAMESPACE_IMPL_START

extern Cut_Man_t * Abc_NtkStartCutManForRewrite( Abc_Ntk_t * pNtk );
extern int Dec_GraphUpdateNetwork( Abc_Obj_t * pRoot, Dec_Graph_t * pGraph, int fUpdateLevel, int nGain );

static int Abc_NtkRewriteShareRootValid( Abc_Obj_t * pNode )
{
    return pNode != NULL && Abc_ObjIsNode(pNode) && !Abc_NodeIsPersistant(pNode) && Abc_ObjFanoutNum(pNode) <= 1000;
}

static Vec_Int_t * Abc_NtkRewriteShareCollectRoots( Abc_Ntk_t * pNtk )
{
    Vec_Int_t * vRootIds;
    Abc_Obj_t * pNode;
    int i, nNodes;
    nNodes = Abc_NtkObjNumMax(pNtk);
    vRootIds = Vec_IntAlloc( nNodes );
    Abc_NtkForEachNode( pNtk, pNode, i )
    {
        if ( i >= nNodes )
            break;
        if ( !Abc_NtkRewriteShareRootValid(pNode) )
            continue;
        Vec_IntPush( vRootIds, Abc_ObjId(pNode) );
    }
    return vRootIds;
}

static Rwr_Profile_t * Abc_NtkRewriteShareBuildProfile( Abc_Ntk_t * pNtk, Rwr_Man_t * pManRwr, int fUpdateLevel, int fUseZeros, int nTopK, Rwr_ShareStats_t * pStats )
{
    Cut_Man_t * pManCut;
    Rwr_Profile_t * pProf;
    Abc_Obj_t * pNode;
    int i, nNodes;
    abctime clk, clkProfile;

    clkProfile = Abc_Clock();
clk = Abc_Clock();
    pManCut = Abc_NtkStartCutManForRewrite( pNtk );
Rwr_ManAddTimeCuts( pManRwr, Abc_Clock() - clk );
    pNtk->pManCut = pManCut;

    pProf = Rwr_ProfileStart( pNtk, nTopK, 0 );

    nNodes = Abc_NtkObjNumMax(pNtk);
    Abc_NtkForEachNode( pNtk, pNode, i )
    {
        if ( i >= nNodes )
            break;
        if ( !Abc_NtkRewriteShareRootValid(pNode) )
            continue;
        Rwr_NodeRewriteProfile( pManRwr, pManCut, pNode, fUpdateLevel, fUseZeros, 0, pProf );
    }
    Rwr_ProfileFinalize( pProf );

    pStats->nProfileBuilds++;
    pStats->timeProfileTotal += Abc_Clock() - clkProfile;
    pStats->nProfileRepeatedSigsLast = Rwr_ProfileRepeatedSigs( pProf );
    pStats->nProfileCrossRootSigsLast = Rwr_ProfileCrossRootSigs( pProf );
    pStats->nProfileRepeatedSigsSum += pStats->nProfileRepeatedSigsLast;
    pStats->nProfileCrossRootSigsSum += pStats->nProfileCrossRootSigsLast;

    Cut_ManStop( pManCut );
    pNtk->pManCut = NULL;
    return pProf;
}

static int Abc_NtkRewriteShareRewriteOne( Abc_Ntk_t * pNtk, Rwr_Man_t * pManRwr, Cut_Man_t * pManCut, Abc_Obj_t * pNode, int fUpdateLevel, int fUseZeros, Rwr_Profile_t * pProf, Rwr_ShareStats_t * pStats, int fUseShareSelect, int nShareDeltaMin, int nScoreMode, int fUseEqualGainShare, int nDestroyProbeThreshold, int fVerbose )
{
    Dec_Graph_t * pGraph;
    Vec_Int_t * vSelectedFaninIds = NULL;
    int nGain, fCompl, nSelectedShare = -1;
    abctime clk;
    (void)pNtk;

    pStats->nRootsProcessed++;
    if ( nDestroyProbeThreshold >= 0 )
        vSelectedFaninIds = Vec_IntAlloc( 4 );
    nGain = Rwr_NodeRewriteShare( pManRwr, pManCut, pNode, fUpdateLevel, fUseZeros, 0, pProf, pStats, fUseShareSelect, nShareDeltaMin, nScoreMode, fUseEqualGainShare, vSelectedFaninIds, nDestroyProbeThreshold >= 0 ? &nSelectedShare : NULL );
    if ( !(nGain > 0 || (nGain == 0 && fUseZeros)) )
    {
        if ( vSelectedFaninIds )
            Vec_IntFree( vSelectedFaninIds );
        return 0;
    }

    pGraph = (Dec_Graph_t *)Rwr_ManReadDecs(pManRwr);
    fCompl = Rwr_ManReadCompl(pManRwr);
    Rwr_ShareProbeDestroyedMffc( pProf, pNode, vSelectedFaninIds, nGain, nSelectedShare, pStats, nDestroyProbeThreshold, fVerbose );
    if ( vSelectedFaninIds )
        Vec_IntFree( vSelectedFaninIds );
    if ( fCompl ) Dec_GraphComplement( pGraph );
clk = Abc_Clock();
    if ( !Dec_GraphUpdateNetwork( pNode, pGraph, fUpdateLevel, nGain ) )
    {
        if ( fCompl ) Dec_GraphComplement( pGraph );
        return -1;
    }
Rwr_ManAddTimeUpdate( pManRwr, Abc_Clock() - clk );
    if ( fCompl ) Dec_GraphComplement( pGraph );
    if ( nGain > 0 )
        pStats->nRewritePos++;
    else
        pStats->nRewriteZero++;
    return 1;
}

int Abc_NtkRewriteShare( Abc_Ntk_t * pNtk, int fUpdateLevel, int fUseZeros, int nTopK, int fUseShareSelect, int nShareDeltaMin, int nScoreMode, int nProfileBlocks, int nDestroyProbeThreshold, int fUseEqualGainShare, int fVerbose )
{
    ProgressBar * pProgress;
    Cut_Man_t * pManCut;
    Rwr_Man_t * pManRwr;
    Rwr_Profile_t * pProf;
    Rwr_ShareStats_t Stats;
    Abc_Obj_t * pNode;
    Vec_Int_t * vRootIds;
    int i, k, nNodes, nRoots, nBlocksEff, b, Start, End, RootId, RetValue = 1;
    long long nActuallyChangedBefore, nRootsProcessedBefore;
    abctime clk, clkStart = Abc_Clock(), clkRewrite, clkBlockRewrite, clkBlockProfile;

    assert( Abc_NtkIsStrash(pNtk) );
    assert( nProfileBlocks >= 1 );
    memset( &Stats, 0, sizeof(Rwr_ShareStats_t) );
    Stats.fUseShareSelect = fUseShareSelect;
    Stats.nShareDeltaMin = nShareDeltaMin;
    Stats.nScoreMode = nScoreMode;
    Stats.nProfileBlocksRequested = nProfileBlocks;
    Stats.nProfileBlocksEffective = 1;
    Stats.fDestroyProbe = nDestroyProbeThreshold >= 0;
    Stats.nDestroyProbeThreshold = nDestroyProbeThreshold;
    Stats.fUseEqualGainShare = fUseEqualGainShare;

    Abc_AigCleanup((Abc_Aig_t *)pNtk->pManFunc);

    pManRwr = Rwr_ManStart( 0 );
    if ( pManRwr == NULL )
        return 0;
    if ( fUpdateLevel )
        Abc_NtkStartReverseLevels( pNtk, 0 );

    vRootIds = Abc_NtkRewriteShareCollectRoots( pNtk );
    nRoots = Vec_IntSize( vRootIds );
    if ( nRoots == 0 )
    {
        Stats.nProfileBlocksEffective = 0;
        if ( fVerbose )
            Rwr_ShareStatsPrint( &Stats );
        Vec_IntFree( vRootIds );
        goto finish;
    }

    if ( nProfileBlocks == 1 )
    {
        Vec_IntFree( vRootIds );
        pProf = Abc_NtkRewriteShareBuildProfile( pNtk, pManRwr, fUpdateLevel, fUseZeros, nTopK, &Stats );
        clkRewrite = Abc_Clock();
clk = Abc_Clock();
        pManCut = Abc_NtkStartCutManForRewrite( pNtk );
Rwr_ManAddTimeCuts( pManRwr, Abc_Clock() - clk );
        pNtk->pManCut = pManCut;

        pManRwr->nNodesBeg = Abc_NtkNodeNum(pNtk);
        nNodes = Abc_NtkObjNumMax(pNtk);
        pProgress = Extra_ProgressBarStart( stdout, nNodes );
        Abc_NtkForEachNode( pNtk, pNode, i )
        {
            Extra_ProgressBarUpdate( pProgress, i, NULL );
            if ( i >= nNodes )
                break;
            if ( !Abc_NtkRewriteShareRootValid(pNode) )
                continue;
            Stats.nRootsScheduled++;

            if ( Abc_NtkRewriteShareRewriteOne( pNtk, pManRwr, pManCut, pNode, fUpdateLevel, fUseZeros, pProf, &Stats, fUseShareSelect, nShareDeltaMin, nScoreMode, fUseEqualGainShare, nDestroyProbeThreshold, fVerbose ) < 0 )
            {
                RetValue = -1;
                break;
            }
        }
        Extra_ProgressBarStop( pProgress );
        Stats.timeRewriteTotal += Abc_Clock() - clkRewrite;
        Cut_ManStop( pManCut );
        pNtk->pManCut = NULL;
        if ( fVerbose )
            Rwr_ShareStatsPrint( &Stats );
        Rwr_ProfileStop( pProf );
    }
    else
    {
        nBlocksEff = nProfileBlocks < nRoots ? nProfileBlocks : nRoots;
        Stats.nProfileBlocksEffective = nBlocksEff;
        Stats.nRootsScheduled = nRoots;
        pManRwr->nNodesBeg = Abc_NtkNodeNum(pNtk);

        /*
         * Current implementation rebuilds the profile over the current AIG at each block boundary,
         * while rewrite updates are applied only to the pre-collected original roots in the current block.
         */
        pProgress = nRoots ? Extra_ProgressBarStart( stdout, nRoots ) : NULL;
        for ( b = 0; b < nBlocksEff && RetValue >= 0; b++ )
        {
            Start = (int)(((long long)b * nRoots) / nBlocksEff);
            End   = (int)(((long long)(b + 1) * nRoots) / nBlocksEff);
            nActuallyChangedBefore = Stats.nActuallyChangedRoots;
            nRootsProcessedBefore = Stats.nRootsProcessed;

            clkBlockProfile = Abc_Clock();
            pProf = Abc_NtkRewriteShareBuildProfile( pNtk, pManRwr, fUpdateLevel, fUseZeros, nTopK, &Stats );
            clkBlockProfile = Abc_Clock() - clkBlockProfile;

            clkBlockRewrite = Abc_Clock();
clk = Abc_Clock();
            pManCut = Abc_NtkStartCutManForRewrite( pNtk );
Rwr_ManAddTimeCuts( pManRwr, Abc_Clock() - clk );
            pNtk->pManCut = pManCut;

            for ( k = Start; k < End; k++ )
            {
                if ( pProgress )
                    Extra_ProgressBarUpdate( pProgress, k, NULL );
                RootId = Vec_IntEntry( vRootIds, k );
                pNode = Abc_NtkObj( pNtk, RootId );
                if ( !Abc_NtkRewriteShareRootValid(pNode) )
                {
                    Stats.nRootsSkipped++;
                    continue;
                }
                if ( Abc_NtkRewriteShareRewriteOne( pNtk, pManRwr, pManCut, pNode, fUpdateLevel, fUseZeros, pProf, &Stats, fUseShareSelect, nShareDeltaMin, nScoreMode, fUseEqualGainShare, nDestroyProbeThreshold, fVerbose ) < 0 )
                {
                    RetValue = -1;
                    break;
                }
            }

            Cut_ManStop( pManCut );
            pNtk->pManCut = NULL;
            Stats.timeRewriteTotal += Abc_Clock() - clkBlockRewrite;
            if ( fVerbose )
                printf( "Block %d: roots=%d, processed=%lld, actually_changed=%lld, profile_time=%.6f, rewrite_time=%.6f\n",
                    b, End - Start, Stats.nRootsProcessed - nRootsProcessedBefore, Stats.nActuallyChangedRoots - nActuallyChangedBefore,
                    1.0 * (double)clkBlockProfile / (double)CLOCKS_PER_SEC,
                    1.0 * (double)(Abc_Clock() - clkBlockRewrite) / (double)CLOCKS_PER_SEC );
            Rwr_ProfileStop( pProf );
        }
        if ( pProgress )
            Extra_ProgressBarStop( pProgress );
        if ( fVerbose )
            Rwr_ShareStatsPrint( &Stats );
        Vec_IntFree( vRootIds );
    }
finish:
Rwr_ManAddTimeTotal( pManRwr, Abc_Clock() - clkStart );

    pManRwr->nNodesEnd = Abc_NtkNodeNum(pNtk);
    Rwr_ManStop( pManRwr );
    pNtk->pManCut = NULL;

    Abc_NtkReassignIds( pNtk );
    if ( RetValue >= 0 )
    {
        if ( fUpdateLevel )
            Abc_NtkStopReverseLevels( pNtk );
        else
            Abc_NtkLevel( pNtk );
        if ( !Abc_NtkCheck( pNtk ) )
        {
            printf( "Abc_NtkRewriteShare: The network check has failed.\n" );
            return 0;
        }
    }
    return RetValue;
}

ABC_NAMESPACE_IMPL_END

////////////////////////////////////////////////////////////////////////
///                       END OF FILE                                ///
////////////////////////////////////////////////////////////////////////
