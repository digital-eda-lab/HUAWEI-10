/**CFile****************************************************************

  FileName    [rwrShare.h]

  SystemName  [ABC: Logic synthesis and verification system.]

  PackageName [DAG-aware AIG rewriting package.]

  Synopsis    [Shared-aware rewriting declarations.]

***********************************************************************/

#ifndef ABC__opt__rwr__rwrShare_h
#define ABC__opt__rwr__rwrShare_h

#include "opt/rwr/rwr.h"
#include "bool/dec/dec.h"

ABC_NAMESPACE_HEADER_START

typedef struct Rwr_ShareStats_t_ Rwr_ShareStats_t;

struct Rwr_ShareStats_t_
{
    int       fUseShareSelect;
    int       nShareDeltaMin;
    int       nScoreMode;
    int       nProfileBlocksRequested;
    int       nProfileBlocksEffective;
    int       nProfileBuilds;
    int       fDestroyProbe;
    int       nDestroyProbeThreshold;
    int       fUseEqualGainShare;
    long long nRootsConsidered;
    long long nRootsScheduled;
    long long nRootsProcessed;
    long long nRootsSkipped;
    long long nCandidatesConsidered;
    long long nGainPos;
    long long nGainZero;
    long long nGainNeg;
    long long nRewritePos;
    long long nRewriteZero;
    long long nZeroRootsAnalyzed;
    long long nOriginalShareSum;
    long long nBestShareSum;
    long long nActualShareSum;
    long long nWouldChangeRoots;
    long long nBlockedByThreshold;
    long long nActuallyChangedRoots;
    long long nWouldChangeDeltaSum;
    long long nActualChangeDeltaSum;
    long long nMaxShareDelta;
    long long nShareImprovementSum;
    long long nPosRootsAnalyzed;
    long long nPosWouldChangeRoots;
    long long nPosBlockedByThreshold;
    long long nPosActuallyChangedRoots;
    long long nPosWouldChangeDeltaSum;
    long long nPosActualChangeDeltaSum;
    long long nPosShareImprovementSum;
    long long nProfileRepeatedSigsLast;
    long long nProfileCrossRootSigsLast;
    long long nProfileRepeatedSigsSum;
    long long nProfileCrossRootSigsSum;
    long long nDestroyChecked;
    long long nDestroyDeletedNodes;
    long long nDestroyRawTotal;
    long long nDestroyRootTotal;
    long long nDestroyCrossRootSigsTotal;
    long long nDestroyMaxRaw;
    long long nDestroyMaxRoot;
    long long nDestroyPosChecked;
    long long nDestroyZeroChecked;
    long long nDestroyPosRawTotal;
    long long nDestroyZeroRawTotal;
    long long nDestroyPosRootTotal;
    long long nDestroyZeroRootTotal;
    long long nDestroyHighRoots;
    long long nDestroyHighPosRoots;
    long long nDestroyHighZeroRoots;
    long long nDestroyGain1HighRoots;
    long long nDestroyGain2PlusHighRoots;
    abctime   timeProfileTotal;
    abctime   timeRewriteTotal;
};

extern int        Rwr_NodeRewriteShare( Rwr_Man_t * p, Cut_Man_t * pManCut, Abc_Obj_t * pNode, int fUpdateLevel, int fUseZeros, int fPlaceEnable, Rwr_Profile_t * pProf, Rwr_ShareStats_t * pStats, int fUseShareSelect, int nShareDeltaMin, int nScoreMode, int fUseEqualGainShare, Vec_Int_t * vSelectedFaninIds, int * pSelectedShare );
extern void       Rwr_ShareProbeDestroyedMffc( Rwr_Profile_t * pProf, Abc_Obj_t * pNode, Vec_Int_t * vFaninIds, int nGain, int nSelectedShare, Rwr_ShareStats_t * pStats, int nDestroyProbeThreshold, int fVerbose );
extern Vec_Int_t * Rwr_ProfileGraphAddedSigs( Rwr_Profile_t * p, Dec_Graph_t * pGraph, Vec_Ptr_t * vFaninsCur );
extern int        Rwr_ProfileScoreAddedSigs( Rwr_Profile_t * p, Vec_Int_t * vAddedSigs );
extern int        Rwr_ProfileScoreAddedSigsMode( Rwr_Profile_t * p, Vec_Int_t * vAddedSigs, int ExistingReuse, int nScoreMode );
extern int        Rwr_ProfileObjSigLit( Rwr_Profile_t * pProf, Abc_Obj_t * pObj );
extern int        Rwr_ProfileSigFreq( Rwr_Profile_t * p, int Sig );
extern int        Rwr_ProfileSigRootFreq( Rwr_Profile_t * p, int Sig );
extern int        Rwr_ProfileSigFreqByLit( Rwr_Profile_t * p, int SigLit );
extern int        Rwr_ProfileSigRootFreqByLit( Rwr_Profile_t * p, int SigLit );
extern long long  Rwr_ProfileRepeatedSigs( Rwr_Profile_t * p );
extern long long  Rwr_ProfileCrossRootSigs( Rwr_Profile_t * p );
extern void       Rwr_ShareStatsPrint( Rwr_ShareStats_t * pStats );

ABC_NAMESPACE_HEADER_END

#endif

////////////////////////////////////////////////////////////////////////
///                       END OF FILE                                ///
////////////////////////////////////////////////////////////////////////
