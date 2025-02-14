/*
** @desc CPU SPH 主文件
*/

#include "JSphCpuSingle.h"
#include "JCellDivCpuSingle.h"
#include "JArraysCpu.h"
#include "Functions.h"
#include "JXml.h"
#include "JSphMotion.h"
#include "JPartsLoad4.h"
#include "JSphVisco.h"
#include "JWaveGen.h"
#include "JTimeOut.h"

#include <climits>

using namespace std;


/*
 * @desc 构造器
 */
JSphCpuSingle::JSphCpuSingle() : JSphCpu(false) {
    ClassName = "JSphCpuSingle";
    CellDivSingle = NULL;
    PartsLoaded = NULL;
}

/*
 * @desc 析构函数
 */
JSphCpuSingle::~JSphCpuSingle() {
    delete CellDivSingle;
    CellDivSingle = NULL;
    delete PartsLoaded;
    PartsLoaded = NULL;
}

/*
 * @desc 返回在CPU中保留的内存
 */
llong JSphCpuSingle::GetAllocMemoryCpu() const {
    llong s = JSphCpu::GetAllocMemoryCpu();
    // Reservada en otros objetos
    if (CellDivSingle)s += CellDivSingle->GetAllocMemory();
    if (PartsLoaded)s += PartsLoaded->GetAllocMemory();
    return (s);
}

/*
 * @desc 更新内存，粒子和cell的最大值
 */
void JSphCpuSingle::UpdateMaxValues() {
    MaxParticles = max(MaxParticles, Np);
    if (CellDivSingle)MaxCells = max(MaxCells, CellDivSingle->GetNct());
    llong m = GetAllocMemoryCpu();
    MaxMemoryCpu = max(MaxMemoryCpu, m);
}


/*
 * @desc 载入参数的 Helper
 */
void JSphCpuSingle::LoadConfig(JCfgRun *cfg) {
    const char met[] = "LoadConfig";
    // 载入 OpenMP 配置(多线程)
    // JSphCpu->OmpThreads
    ConfigOmp(cfg);
    // 载入基本参数
    JSph::LoadConfig(cfg);
    Log->Print("**Special case configuration is loaded");
}

/*
 * @desc 加载case和process的粒子
 */
void JSphCpuSingle::LoadCaseParticles() {
    Log->Print("Loading initial state of particles...");
    PartsLoaded = new JPartsLoad4;
    PartsLoaded->LoadParticles(DirCase, CaseName, PartBegin, PartBeginDir);
    PartsLoaded->CheckConfig(CaseNp, CaseNfixed, CaseNmoving, CaseNfloat, CaseNfluid, PeriX, PeriY, PeriZ);
    Log->Printf("Loaded particles: %u", PartsLoaded->GetCount());
    // 收集载入的粒子信息
    Simulate2D = PartsLoaded->GetSimulate2D();
    if (Simulate2D && PeriY)
        RunException("LoadCaseParticles", "Cannot use periodic conditions in Y with 2D simulations");
    CasePosMin = PartsLoaded->GetCasePosMin();
    CasePosMax = PartsLoaded->GetCasePosMax();

    // 计算模拟的实际限制
    if (PartsLoaded->MapSizeLoaded())PartsLoaded->GetMapSize(MapRealPosMin, MapRealPosMax);
    else {
        PartsLoaded->CalculeLimits(double(H) * BORDER_MAP, Dp / 2., PeriX, PeriY, PeriZ, MapRealPosMin, MapRealPosMax);
        ResizeMapLimits();
    }
    if (PartBegin) {
        PartBeginTimeStep = PartsLoaded->GetPartBeginTimeStep();
        PartBeginTotalNp = PartsLoaded->GetPartBeginTotalNp();
    }
    Log->Print(string("MapRealPos(final)=") + fun::Double3gRangeStr(MapRealPosMin, MapRealPosMax));
    MapRealSize = MapRealPosMax - MapRealPosMin;
    Log->Print("**Initial state of particles is loaded");

    //-Configure limits of periodic axes / Configura limites de ejes periodicos
    if (PeriX)PeriXinc.x = -MapRealSize.x;
    if (PeriY)PeriYinc.y = -MapRealSize.y;
    if (PeriZ)PeriZinc.z = -MapRealSize.z;
    // 计算模拟的周期边界限制
    Map_PosMin = MapRealPosMin;
    Map_PosMax = MapRealPosMax;
    float dosh = float(H * 2);
    if (PeriX) {
        Map_PosMin.x = Map_PosMin.x - dosh;
        Map_PosMax.x = Map_PosMax.x + dosh;
    }
    if (PeriY) {
        Map_PosMin.y = Map_PosMin.y - dosh;
        Map_PosMax.y = Map_PosMax.y + dosh;
    }
    if (PeriZ) {
        Map_PosMin.z = Map_PosMin.z - dosh;
        Map_PosMax.z = Map_PosMax.z + dosh;
    }
    Map_Size = Map_PosMax - Map_PosMin;
}

/*
 * @desc 当前域的配置
 */
void JSphCpuSingle::ConfigDomain() {
    const char *met = "ConfigDomain";
    // 计算粒子数量
    Np = PartsLoaded->GetCount();
    Npb = CaseNpb;
    NpbOk = Npb;
    // 为移动和浮动粒子分配固定内存
    AllocCpuMemoryFixed();
    // 分配CPU中的内存以获取粒子
    AllocCpuMemoryParticles(Np, 0);

    // 复制粒子值
    ReserveBasicArraysCpu();
    memcpy(Posc, PartsLoaded->GetPos(), sizeof(tdouble3) * Np);
    memcpy(Idpc, PartsLoaded->GetIdp(), sizeof(unsigned) * Np);
    memcpy(Velrhopc, PartsLoaded->GetVelRhop(), sizeof(tfloat4) * Np);

    // 计算浮动半径
    if (CaseNfloat && PeriActive != 0 && !PartBegin) {
        CalcFloatingRadius(Np, Posc, Idpc);
    }

    // 加载粒子数据
    LoadCodeParticles(Np, Idpc, Codec);

    // 释放内存
    delete PartsLoaded;
    PartsLoaded = NULL;
    // 应用CellOrder的配置
    ConfigCellOrder(CellOrder, Np, Posc, Velrhopc);

    // 配置 cells division
    ConfigCellDivision();
    // 在Map_Cells内部建立本地仿真域并计算DomCellCode
    SelecDomain(TUint3(0, 0, 0), Map_Cells);
    // 计算粒子的初始单元格，并检查是否有意外排除的粒子
    LoadDcellParticles(Np, Codec, Posc, Dcellc);

    // 创建用于在CPU中划分的对象并选择有效的单元模式
    CellDivSingle = new JCellDivCpuSingle(Stable, FtCount != 0, PeriActive, CellOrder, CellMode, Scell, Map_PosMin,
                                          Map_PosMax, Map_Cells, CaseNbound, CaseNfixed, CaseNpb, Log, DirOut);
    CellDivSingle->DefineDomain(DomCellCode, DomCelIni, DomCelFin, DomPosMin, DomPosMax);
    ConfigCellDiv((JCellDivCpu *) CellDivSingle);

    ConfigSaveData(0, 1, "");

    // 为cell重新排序粒子
    BoundChanged = true;
    RunCellDivide(true);
}

/*
 * @desc 为CPU中的粒子保留的Redimension空间，测量使用TMC_SuResizeNp消耗的时间。完成后，更新划分。
 */
void JSphCpuSingle::ResizeParticlesSize(unsigned newsize, float oversize, bool updatedivide) {
    TmcStart(Timers, TMC_SuResizeNp);
    newsize += (oversize > 0 ? unsigned(oversize * newsize) : 0);
    ResizeCpuMemoryParticles(newsize);
    TmcStop(Timers, TMC_SuResizeNp);
    if (updatedivide)RunCellDivide(true);
}

/*
 * @desc
 * 创建要复制的新的周期性粒子列表
 * 具有稳定的激活重新排序的周期性粒子列表
 */
unsigned JSphCpuSingle::PeriodicMakeList(unsigned n, unsigned pini, bool stable, unsigned nmax, tdouble3 perinc,
                                         const tdouble3 *pos, const word *code, unsigned *listp) const {
    unsigned count = 0;
    if (n) {
        //-Initialize size of list lsph to zero / Inicializa tama�o de lista lspg a cero.
        listp[nmax] = 0;
        for (unsigned p = 0; p < n; p++) {
            const unsigned p2 = p + pini;
            //-Keep normal or periodic particles / Se queda con particulas normales o periodicas.
            if (CODE_GetSpecialValue(code[p2]) <= CODE_PERIODIC) {
                //-Get particle position / Obtiene posicion de particula.
                const tdouble3 ps = pos[p2];
                tdouble3 ps2 = ps + perinc;
                if (Map_PosMin <= ps2 && ps2 < Map_PosMax) {
                    unsigned cp = listp[nmax];
                    listp[nmax]++;
                    if (cp < nmax)listp[cp] = p2;
                }
                ps2 = ps - perinc;
                if (Map_PosMin <= ps2 && ps2 < Map_PosMax) {
                    unsigned cp = listp[nmax];
                    listp[nmax]++;
                    if (cp < nmax)listp[cp] = (p2 | 0x80000000);
                }
            }
        }
        count = listp[nmax];
        //-Reorder list if it is valid and stability is activated / Reordena lista si es valida y stable esta activado.
        if (stable && count && count <= nmax) {
            //-Don't make mistaje because at the moment the list is not created using OpenMP / No hace falta porque de momento no se crea la lista usando OpenMP.
        }
    }
    return (count);
}

/*
 * @desc
 * 应用位移复制指定的粒子位置
 * 重复的粒子被认为总是有效的并且在域内
 * 此内核适用于单CPU和多CPU，因为计算是从domposmin开始的
 * 控制单元坐标不超过最大值
 */
void JSphCpuSingle::PeriodicDuplicatePos(unsigned pnew, unsigned pcopy, bool inverse, double dx, double dy, double dz,
                                         tuint3 cellmax, tdouble3 *pos, unsigned *dcell) const {
    //-Get pos of particle to be duplicated / Obtiene pos de particula a duplicar.
    tdouble3 ps = pos[pcopy];
    //-Apply displacement / Aplica desplazamiento.
    ps.x += (inverse ? -dx : dx);
    ps.y += (inverse ? -dy : dy);
    ps.z += (inverse ? -dz : dz);
    //-Calculate coordinates of cell inside of domain / Calcula coordendas de celda dentro de dominio.
    unsigned cx = unsigned((ps.x - DomPosMin.x) / Scell);
    unsigned cy = unsigned((ps.y - DomPosMin.y) / Scell);
    unsigned cz = unsigned((ps.z - DomPosMin.z) / Scell);
    //-Adjust coordinates of cell is they exceed maximum / Ajusta las coordendas de celda si sobrepasan el maximo.
    cx = (cx <= cellmax.x ? cx : cellmax.x);
    cy = (cy <= cellmax.y ? cy : cellmax.y);
    cz = (cz <= cellmax.z ? cz : cellmax.z);
    //-Record position and cell of new particles /  Graba posicion y celda de nuevas particulas.
    pos[pnew] = ps;
    dcell[pnew] = PC__Cell(DomCellCode, cx, cy, cz);
}

/*
 * @desc
 * 从要复制的粒子列表开始创建周期性粒子
 * 假设所有的粒子都是有效的
 * 此内核适用于单CPU和多CPU，因为它使用 domposmin
 */
void JSphCpuSingle::PeriodicDuplicateVerlet(unsigned np, unsigned pini, tuint3 cellmax, tdouble3 perinc,
                                            const unsigned *listp, unsigned *idp, word *code, unsigned *dcell,
                                            tdouble3 *pos, tfloat4 *velrhop, tsymatrix3f *spstau,
                                            tfloat4 *velrhopm1) const {
    const int n = int(np);
#ifdef _WITHOMP
#pragma omp parallel for schedule (static) if(n>LIMIT_COMPUTELIGHT_OMP)
#endif
    for (int p = 0; p < n; p++) {
        const unsigned pnew = unsigned(p) + pini;
        const unsigned rp = listp[p];
        const unsigned pcopy = (rp & 0x7FFFFFFF);
        //-Adjust position and cell of new particle / Ajusta posicion y celda de nueva particula.
        PeriodicDuplicatePos(pnew, pcopy, (rp >= 0x80000000), perinc.x, perinc.y, perinc.z, cellmax, pos, dcell);
        //-Copy the rest of the values / Copia el resto de datos.
        idp[pnew] = idp[pcopy];
        code[pnew] = CODE_SetPeriodic(code[pcopy]);
        velrhop[pnew] = velrhop[pcopy];
        velrhopm1[pnew] = velrhopm1[pcopy];
        if (spstau)spstau[pnew] = spstau[pcopy];
    }
}

/*
 * @desc
 * 从要复制的粒子列表开始创建周期性粒子
 * 假设所有的粒子都是有效的
 * 此内核适用于单CPU和多CPU，因为它使用 domposmin
 */
void JSphCpuSingle::PeriodicDuplicateSymplectic(unsigned np, unsigned pini, tuint3 cellmax, tdouble3 perinc,
                                                const unsigned *listp, unsigned *idp, word *code, unsigned *dcell,
                                                tdouble3 *pos, tfloat4 *velrhop, tsymatrix3f *spstau, tdouble3 *pospre,
                                                tfloat4 *velrhoppre) const {
    const int n = int(np);
#ifdef _WITHOMP
#pragma omp parallel for schedule (static) if(n>LIMIT_COMPUTELIGHT_OMP)
#endif
    for (int p = 0; p < n; p++) {
        const unsigned pnew = unsigned(p) + pini;
        const unsigned rp = listp[p];
        const unsigned pcopy = (rp & 0x7FFFFFFF);
        //-Adjust position and cell of new particle / Ajusta posicion y celda de nueva particula.
        PeriodicDuplicatePos(pnew, pcopy, (rp >= 0x80000000), perinc.x, perinc.y, perinc.z, cellmax, pos, dcell);
        //-Copy the rest of the values / Copia el resto de datos.
        idp[pnew] = idp[pcopy];
        code[pnew] = CODE_SetPeriodic(code[pcopy]);
        velrhop[pnew] = velrhop[pcopy];
        if (pospre)pospre[pnew] = pospre[pcopy];
        if (velrhoppre)velrhoppre[pnew] = velrhoppre[pcopy];
        if (spstau)spstau[pnew] = spstau[pcopy];
    }
}

/*
 * @desc
 * 为周期性条件创建重复的粒子
 * 创建新的周期性粒子并将其标记为忽略
 * 从周围的Np开始创建新的周期性粒子
 * 首先是NpbPer边界
 * 然后是NpfPer流体
 * 那些离开的粒子的Np也包含了新的周期性的粒子
 */
void JSphCpuSingle::RunPeriodic() {
    const char met[] = "RunPeriodic";
    TmcStart(Timers, TMC_SuPeriodic);
    //-Keep number of present periodic / Guarda numero de periodicas actuales.
    NpfPerM1 = NpfPer;
    NpbPerM1 = NpbPer;
    //-Mark present periodic particles to ignore / Marca periodicas actuales para ignorar.
    for (unsigned p = 0; p < Np; p++) {
        const word rcode = Codec[p];
        if (CODE_GetSpecialValue(rcode) == CODE_PERIODIC)Codec[p] = CODE_SetOutIgnore(rcode);
    }
    //-Create new periodic particles / Crea las nuevas periodicas.
    const unsigned npb0 = Npb;
    const unsigned npf0 = Np - Npb;
    const unsigned np0 = Np;
    NpbPer = NpfPer = 0;
    BoundChanged = true;
    for (unsigned ctype = 0; ctype < 2; ctype++) {//-0:bound, 1:fluid+floating.
        //-Calculat range of particles to be examined (bound or fluid) / Calcula rango de particulas a examinar (bound o fluid).
        const unsigned pini = (ctype ? npb0 : 0);
        const unsigned num = (ctype ? npf0 : npb0);
        //-Search for periodic in each direction (X, Y, or Z) / Busca periodicas en cada eje (X, Y e Z).
        for (unsigned cper = 0; cper < 3; cper++)
            if ((cper == 0 && PeriActive & 1) || (cper == 1 && PeriActive & 2) || (cper == 2 && PeriActive & 4)) {
                tdouble3 perinc = (cper == 0 ? PeriXinc : (cper == 1 ? PeriYinc : PeriZinc));
                //-Primero busca en la lista de periodicas nuevas y despues en la lista inicial de particulas (necesario para periodicas en mas de un eje).
                //-First  search in the list of new periodic particles and then in the initial list of particles (this is needed for periodic particles in more than one direction).
                for (unsigned cblock = 0; cblock < 2; cblock++) {//-0:periodicas nuevas, 1:particulas originales
                    const unsigned nper = (ctype ? NpfPer
                                                 : NpbPer); //-Number of new periodic particles of type to be processed / Numero de periodicas nuevas del tipo a procesar.
                    const unsigned pini2 = (cblock ? pini : Np - nper);
                    const unsigned num2 = (cblock ? num : nper);
                    //-Repite la busqueda si la memoria disponible resulto insuficiente y hubo que aumentarla.
                    //-Repeat the search if the resulting memory available is insufficient and it had to be increased.
                    bool run = true;
                    while (run && num2) {
                        //-Reserve memory to create list of periodic particles / Reserva memoria para crear lista de particulas periodicas.
                        unsigned *listp = ArraysCpu->ReserveUint();
                        unsigned nmax = CpuParticlesSize -
                                        1; //-Maximmum number of particles that fit in the list / Numero maximo de particulas que caben en la lista.
                        //-Generate list of new periodic particles / Genera lista de nuevas periodicas.
                        if (Np >= 0x80000000)
                            RunException(met,
                                         "The number of particles is too big.");//-Because the last bit is used to mark the direction in which a new periodic particle is created / Pq el ultimo bit se usa para marcar el sentido en que se crea la nueva periodica.
                        unsigned count = PeriodicMakeList(num2, pini2, Stable, nmax, perinc, Posc, Codec, listp);
                        //-Redimensiona memoria para particulas si no hay espacio suficiente y repite el proceso de busqueda.
                        //-Redimension memory for particles if there is insufficient space and repeat the search process.
                        if (count > nmax || count + Np > CpuParticlesSize) {
                            ArraysCpu->Free(listp);
                            listp = NULL;
                            TmcStop(Timers, TMC_SuPeriodic);
                            ResizeParticlesSize(Np + count, PERIODIC_OVERMEMORYNP, false);
                            TmcStart(Timers, TMC_SuPeriodic);
                        } else {
                            run = false;
                            //-Crea nuevas particulas periodicas duplicando las particulas de la lista.
                            //-Create new duplicate periodic particles in the list
                            if (TStep == STEP_Verlet)
                                PeriodicDuplicateVerlet(count, Np, DomCells, perinc, listp, Idpc, Codec, Dcellc, Posc,
                                                        Velrhopc, SpsTauc, VelrhopM1c);
                            if (TStep == STEP_Symplectic) {
                                if ((PosPrec || VelrhopPrec) && (!PosPrec || !VelrhopPrec))
                                    RunException(met, "Symplectic data is invalid.");
                                PeriodicDuplicateSymplectic(count, Np, DomCells, perinc, listp, Idpc, Codec, Dcellc,
                                                            Posc, Velrhopc, SpsTauc, PosPrec, VelrhopPrec);
                            }

                            //-Free the list and update the number of particles / Libera lista y actualiza numero de particulas.
                            ArraysCpu->Free(listp);
                            listp = NULL;
                            Np += count;
                            //-Update number of new periodic particles / Actualiza numero de periodicas nuevas.
                            if (!ctype)NpbPer += count;
                            else NpfPer += count;
                        }
                    }
                }
            }
    }
    TmcStop(Timers, TMC_SuPeriodic);
}

/*
 * @desc 执行cell中的粒子分割
 */
void JSphCpuSingle::RunCellDivide(bool updateperiodic) {
    const char met[] = "RunCellDivide";
    //-Create new periodic particles & mark the old ones to be ignored / Crea nuevas particulas periodicas y marca las viejas para ignorarlas.
    if (updateperiodic && PeriActive)RunPeriodic();

    //-Initial Divide / Inicia Divide.
    CellDivSingle->Divide(Npb, Np - Npb - NpbPer - NpfPer, NpbPer, NpfPer, BoundChanged, Dcellc, Codec, Idpc, Posc,
                          Timers);

    //-Order particle data / Ordena datos de particulas
    TmcStart(Timers, TMC_NlSortData);
    CellDivSingle->SortArray(Idpc);
    CellDivSingle->SortArray(Codec);
    CellDivSingle->SortArray(Dcellc);
    CellDivSingle->SortArray(Posc);
    CellDivSingle->SortArray(Velrhopc);
    if (TStep == STEP_Verlet) {
        CellDivSingle->SortArray(VelrhopM1c);
    } else if (TStep == STEP_Symplectic && (PosPrec ||
                                            VelrhopPrec)) {//In reality, this is only necessary in divide for corrector, not in predictor??? / En realidad solo es necesario en el divide del corrector, no en el predictor???
        if (!PosPrec || !VelrhopPrec)RunException(met, "Symplectic data is invalid.");
        CellDivSingle->SortArray(PosPrec);
        CellDivSingle->SortArray(VelrhopPrec);
    }
    if (TVisco == VISCO_LaminarSPS)CellDivSingle->SortArray(SpsTauc);

    //-Collect divide data / Recupera datos del divide.
    Np = CellDivSingle->GetNpFinal();
    Npb = CellDivSingle->GetNpbFinal();
    NpbOk = Npb - CellDivSingle->GetNpbIgnore();
    //-Collect position of floating particles / Recupera posiciones de floatings.
    if (CaseNfloat)CalcRidp(PeriActive != 0, Np - Npb, Npb, CaseNpb, CaseNpb + CaseNfloat, Codec, Idpc, FtRidp);
    TmcStop(Timers, TMC_NlSortData);

    //-Gestion de particulas excluidas (solo fluid pq si alguna bound es excluida se genera excepcion en Divide()).
    //-Control of excluded particles (only fluid because if some bound is excluded ti generates an exception in Divide()).
    TmcStart(Timers, TMC_NlOutCheck);
    unsigned npfout = CellDivSingle->GetNpOut();
    if (npfout) {
        unsigned *idp = ArraysCpu->ReserveUint();
        tdouble3 *pos = ArraysCpu->ReserveDouble3();
        tfloat3 *vel = ArraysCpu->ReserveFloat3();
        float *rhop = ArraysCpu->ReserveFloat();
        unsigned num = GetParticlesData(npfout, Np, true, false, idp, pos, vel, rhop, NULL);
        AddParticlesOut(npfout, idp, pos, vel, rhop, CellDivSingle->GetNpfOutRhop(), CellDivSingle->GetNpfOutMove());
        ArraysCpu->Free(idp);
        ArraysCpu->Free(pos);
        ArraysCpu->Free(vel);
        ArraysCpu->Free(rhop);
    }
    TmcStop(Timers, TMC_NlOutCheck);
    BoundChanged = false;
}

/*
 * @desc 返回相互作用的cell限制
 */
void JSphCpuSingle::GetInteractionCells(unsigned rcell, int hdiv, const tint4 &nc, const tint3 &cellzero, int &cxini,
                                        int &cxfin, int &yini, int &yfin, int &zini, int &zfin) const {
    //-Get interaction limits / Obtiene limites de interaccion
    const int cx = PC__Cellx(DomCellCode, rcell) - cellzero.x;
    const int cy = PC__Celly(DomCellCode, rcell) - cellzero.y;
    const int cz = PC__Cellz(DomCellCode, rcell) - cellzero.z;
    //-code for hdiv 1 or 2 but not zero / Codigo para hdiv 1 o 2 pero no cero.
    cxini = cx - min(cx, hdiv);
    cxfin = cx + min(nc.x - cx - 1, hdiv) + 1;
    yini = cy - min(cy, hdiv);
    yfin = cy + min(nc.y - cy - 1, hdiv) + 1;
    zini = cz - min(cz, hdiv);
    zfin = cz + min(nc.z - cz - 1, hdiv) + 1;
}

/*
 * @desc 计算相互作用受力
 */
void JSphCpuSingle::Interaction_Forces(TpInter tinter) {
    const char met[] = "Interaction_Forces";
    PreInteraction_Forces(tinter);
    TmcStart(Timers, TMC_CfForces);

    // 流体/束缚和束缚流体（力和DEM）的相互作用
    float viscdt = 0;
    if (Psimple) {
        JSphCpu::InteractionSimple_Forces(Np, Npb, NpbOk, CellDivSingle->GetNcells(), CellDivSingle->GetBeginCell(),
                                          CellDivSingle->GetCellDomainMin(), Dcellc, PsPosc, Velrhopc, Idpc, Codec,
                                          Pressc, viscdt, Arc, Acec, Deltac, SpsTauc, SpsGradvelc, ShiftPosc,
                                          ShiftDetectc);
    } else {
        JSphCpu::Interaction_Forces(Np, Npb, NpbOk, CellDivSingle->GetNcells(), CellDivSingle->GetBeginCell(),
                                    CellDivSingle->GetCellDomainMin(), Dcellc, Posc, Velrhopc, Idpc, Codec, Pressc,
                                    viscdt, Arc, Acec, Deltac, SpsTauc, SpsGradvelc, ShiftPosc, ShiftDetectc);
    }

    // 对于二维模拟，将第二个分量归零
    if (Simulate2D) {
        const int ini = int(Npb), fin = int(Np), npf = int(Np - Npb);
#ifdef _WITHOMP
#pragma omp parallel for schedule (static) if(npf>LIMIT_COMPUTELIGHT_OMP)
#endif
        for (int p = ini; p < fin; p++)Acec[p].y = 0;
    }

    // 将Delta-SPH更正添加到Arg []
    if (Deltac) {
        const int ini = int(Npb), fin = int(Np), npf = int(Np - Npb);
#ifdef _WITHOMP
#pragma omp parallel for schedule (static) if(npf>LIMIT_COMPUTELIGHT_OMP)
#endif
        for (int p = ini; p < fin; p++)if (Deltac[p] != FLT_MAX)Arc[p] += Deltac[p];
    }

    // 计算 ViscDt 的最大值
    ViscDtMax = viscdt;
    // 计算 Ace 的最大值
    AceMax = ComputeAceMaxOmp(PeriActive != 0, Np - Npb, Acec + Npb, Codec + Npb);

    TmcStop(Timers, TMC_CfForces);
}

/*
 * @desc 返回 ace（模数）的最大值。
 */
double
JSphCpuSingle::ComputeAceMaxSeq(const bool checkcodenormal, unsigned np, const tfloat3 *ace, const word *code) const {
    float acemax = 0;
    const int n = int(np);
    //-With periodic conditions ignore periodic particles / Con condiciones periodicas ignora las particulas periodicas.
    for (int p = 0; p < n; p++)
        if (!checkcodenormal || CODE_GetSpecialValue(code[p]) == CODE_NORMAL) {
            const tfloat3 a = ace[p];
            const float a2 = a.x * a.x + a.y * a.y + a.z * a.z;
            acemax = max(acemax, a2);
        }
    return (sqrt(double(acemax)));
}

/*
 * @desc 返回使用 OpenMP 时 ace（模数）的最大值。
 */
double
JSphCpuSingle::ComputeAceMaxOmp(const bool checkcodenormal, unsigned np, const tfloat3 *ace, const word *code) const {
    const char met[] = "ComputeAceMaxOmp";
    double acemax = 0;
#ifdef _WITHOMP
    if (np > LIMIT_COMPUTELIGHT_OMP) {
        const int n = int(np);
        if (n < 0)RunException(met, "Number of values is too big.");
        float amax = 0;
#pragma omp parallel
        {
            float amax2 = 0;
#pragma omp for nowait
            for (int p = 0; p < n; ++p) {
                //-With periodic conditions ignore periodic particles / Con condiciones periodicas ignora las particulas periodicas.
                if (!checkcodenormal || CODE_GetSpecialValue(code[p]) == CODE_NORMAL) {
                    const tfloat3 a = ace[p];
                    const float a2 = a.x * a.x + a.y * a.y + a.z * a.z;
                    if (amax2 < a2)amax2 = a2;
                }
            }
#pragma omp critical
            {
                if (amax < amax2)amax = amax2;
            }
        }
        //-Guarda resultado.
        acemax = sqrt(double(amax));
    } else if (np)acemax = ComputeAceMaxSeq(checkcodenormal, np, ace, code);
#else
    if(np)acemax=ComputeAceMaxSeq(checkcodenormal,np,ace,code);
#endif
    return (acemax);
}

/*
 * @desc 根据受力执行粒子的交互和更新, 使用 Verlet 执行计算
 */
double JSphCpuSingle::ComputeStep_Ver() {
    Interaction_Forces(INTER_Forces);    //-Interaction / Interaccion
    const double dt = DtVariable(true);    //-Calculate new dt / Calcula nuevo dt
    DemDtForce = dt;                       //(DEM)
    if (TShifting)RunShifting(dt);        //-Shifting
    ComputeVerlet(dt);                   //-Update particles using Verlet / Actualiza particulas usando Verlet
    if (CaseNfloat)RunFloating(dt, false); //-Control of floating bodies / Gestion de floating bodies
    PosInteraction_Forces();             //-Free memory used for interaction / Libera memoria de interaccion
    return (dt);
}

/*
 * @desc 根据受力执行粒子的交互和更新, 使用 Symplectic 执行计算
 */
double JSphCpuSingle::ComputeStep_Sym() {
    const double dt = DtPre;
    //-Predictor
    //-----------
    DemDtForce = dt * 0.5f;                     //(DEM)
    Interaction_Forces(INTER_Forces);       //-Interaction / Interaccion
    const double ddt_p = DtVariable(false);   //-Calculate dt of predictor step / Calcula dt del predictor
    if (TShifting)RunShifting(dt * .5);        //-Shifting
    ComputeSymplecticPre(
            dt);               //-Apply Symplectic-Predictor to particles / Aplica Symplectic-Predictor a las particulas
    if (CaseNfloat)RunFloating(dt * .5, true);  //-Control of floating bodies / Gestion de floating bodies
    PosInteraction_Forces();                //-Free memory used for interaction / Libera memoria de interaccion
    //-Corrector
    //-----------
    DemDtForce = dt;                          //(DEM)
    RunCellDivide(true);
    Interaction_Forces(INTER_ForcesCorr);   //Interaction / Interaccion
    const double ddt_c = DtVariable(true);    //-Calculate dt of corrector step / Calcula dt del corrector
    if (TShifting)RunShifting(dt);           //-Shifting
    ComputeSymplecticCorr(
            dt);              //-Apply Symplectic-Corrector to particles / Aplica Symplectic-Corrector a las particulas
    if (CaseNfloat)RunFloating(dt, false);    //-Control of floating bodies / Gestion de floating bodies
    PosInteraction_Forces();                //-Free memory used for interaction / Libera memoria de interaccion

    DtPre = min(ddt_p, ddt_c);                 //-Calcula el dt para el siguiente ComputeStep
    return (dt);
}

/*
 * @desc 根据周期性条件计算浮动粒子与中心之间的距离
 */
tfloat3 JSphCpuSingle::FtPeriodicDist(const tdouble3 &pos, const tdouble3 &center, float radius) const {
    tdouble3 distd = (pos - center);
    if (PeriX && fabs(distd.x) > radius) {
        if (distd.x > 0)distd = distd + PeriXinc;
        else distd = distd - PeriXinc;
    }
    if (PeriY && fabs(distd.y) > radius) {
        if (distd.y > 0)distd = distd + PeriYinc;
        else distd = distd - PeriYinc;
    }
    if (PeriZ && fabs(distd.z) > radius) {
        if (distd.z > 0)distd = distd + PeriZinc;
        else distd = distd - PeriZinc;
    }
    return (ToTFloat3(distd));
}

/*
 * @desc 计算浮动物体周围的力
 */
void JSphCpuSingle::FtCalcForces(StFtoForces *ftoforces) const {
    const int ftcount = int(FtCount);
#ifdef _WITHOMP
#pragma omp parallel for schedule (guided)
#endif
    for (int cf = 0; cf < ftcount; cf++) {
        const StFloatingData fobj = FtObjs[cf];
        const unsigned fpini = fobj.begin - CaseNpb;
        const unsigned fpfin = fpini + fobj.count;
        const float fradius = fobj.radius;
        const tdouble3 fcenter = fobj.center;
        const float fmassp = fobj.massp;
        //-Computes traslational and rotational velocities.
        tfloat3 face = TFloat3(0);
        tfloat3 fomegavel = TFloat3(0);
        tmatrix3f inert = TMatrix3f(0, 0, 0, 0, 0, 0, 0, 0, 0);
        //-Calculate summation: face, fomegavel & inert / Calcula sumatorios: face, fomegavel y inert.
        for (unsigned fp = fpini; fp < fpfin; fp++) {
            int p = FtRidp[fp];
            //-Ace is initialised with the value of the gravity for all particles.
            float acex = Acec[p].x - Gravity.x, acey = Acec[p].y - Gravity.y, acez = Acec[p].z - Gravity.z;
            face.x += acex;
            face.y += acey;
            face.z += acez;
            tfloat3 dist = (PeriActive ? FtPeriodicDist(Posc[p], fcenter, fradius) : ToTFloat3(Posc[p] - fcenter));
            fomegavel.x += acez * dist.y - acey * dist.z;
            fomegavel.y += acex * dist.z - acez * dist.x;
            fomegavel.z += acey * dist.x - acex * dist.y;
            //inertia tensor
            inert.a11 += (float) (dist.y * dist.y + dist.z * dist.z) * fmassp;
            inert.a12 += (float) -(dist.x * dist.y) * fmassp;
            inert.a13 += (float) -(dist.x * dist.z) * fmassp;
            inert.a21 += (float) -(dist.x * dist.y) * fmassp;
            inert.a22 += (float) (dist.x * dist.x + dist.z * dist.z) * fmassp;
            inert.a23 += (float) -(dist.y * dist.z) * fmassp;
            inert.a31 += (float) -(dist.x * dist.z) * fmassp;
            inert.a32 += (float) -(dist.y * dist.z) * fmassp;
            inert.a33 += (float) (dist.x * dist.x + dist.y * dist.y) * fmassp;
        }
        //-Calculates the inverse of the intertia matrix to compute the I^-1 * L= W
        tmatrix3f invinert = TMatrix3f(0, 0, 0, 0, 0, 0, 0, 0, 0);
        const float detiner = (inert.a11 * inert.a22 * inert.a33 + inert.a12 * inert.a23 * inert.a31 +
                               inert.a21 * inert.a32 * inert.a13 -
                               (inert.a31 * inert.a22 * inert.a13 + inert.a21 * inert.a12 * inert.a33 +
                                inert.a23 * inert.a32 * inert.a11));
        if (detiner) {
            invinert.a11 = (inert.a22 * inert.a33 - inert.a23 * inert.a32) / detiner;
            invinert.a12 = -(inert.a12 * inert.a33 - inert.a13 * inert.a32) / detiner;
            invinert.a13 = (inert.a12 * inert.a23 - inert.a13 * inert.a22) / detiner;
            invinert.a21 = -(inert.a21 * inert.a33 - inert.a23 * inert.a31) / detiner;
            invinert.a22 = (inert.a11 * inert.a33 - inert.a13 * inert.a31) / detiner;
            invinert.a23 = -(inert.a11 * inert.a23 - inert.a13 * inert.a21) / detiner;
            invinert.a31 = (inert.a21 * inert.a32 - inert.a22 * inert.a31) / detiner;
            invinert.a32 = -(inert.a11 * inert.a32 - inert.a12 * inert.a31) / detiner;
            invinert.a33 = (inert.a11 * inert.a22 - inert.a12 * inert.a21) / detiner;
        }
        //-Calculate omega starting from fomegavel & invinert / Calcula omega a partir de fomegavel y invinert.
        {
            tfloat3 omega;
            omega.x = (fomegavel.x * invinert.a11 + fomegavel.y * invinert.a12 + fomegavel.z * invinert.a13);
            omega.y = (fomegavel.x * invinert.a21 + fomegavel.y * invinert.a22 + fomegavel.z * invinert.a23);
            omega.z = (fomegavel.x * invinert.a31 + fomegavel.y * invinert.a32 + fomegavel.z * invinert.a33);
            fomegavel = omega;
        }
        //-Keep result in ftoforces[] / Guarda resultados en ftoforces[].
        ftoforces[cf].face = face;
        ftoforces[cf].fomegavel = fomegavel;
    }
}

/*
 * @desc 处理浮动对象
 */
void JSphCpuSingle::RunFloating(double dt, bool predictor) {
    const char met[] = "RunFloating";
    if (TimeStep >=
        FtPause) {//-This is used because if FtPause=0 in symplectic-predictor, code would not enter clause / -Se usa >= pq si FtPause es cero en symplectic-predictor no entraria.
        TmcStart(Timers, TMC_SuFloating);
        //-Calculate forces around floating objects / Calcula fuerzas sobre floatings.
        FtCalcForces(FtoForces);

        //-Apply movement around floating objects / Aplica movimiento sobre floatings.
        const int ftcount = int(FtCount);
#ifdef _WITHOMP
#pragma omp parallel for schedule (guided)
#endif
        for (int cf = 0; cf < ftcount; cf++) {
            //-Get Floating object values / Obtiene datos de floating.
            const StFloatingData fobj = FtObjs[cf];
            //-Compute force face / Calculo de face.
            const float fmass = fobj.mass;
            tfloat3 face = FtoForces[cf].face;
            face.x = (face.x + fmass * Gravity.x) / fmass;
            face.y = (face.y + fmass * Gravity.y) / fmass;
            face.z = (face.z + fmass * Gravity.z) / fmass;
            //-Compute fomega / Calculo de fomega.
            tfloat3 fomega = fobj.fomega;
            {
                const tfloat3 omega = FtoForces[cf].fomegavel;
                fomega.x = float(dt * omega.x + fomega.x);
                fomega.y = float(dt * omega.y + fomega.y);
                fomega.z = float(dt * omega.z + fomega.z);
            }
            tfloat3 fvel = fobj.fvel;
            //-Zero components for 2-D simulation / Anula componentes para 2D.
            if (Simulate2D) {
                face.y = 0;
                fomega.x = 0;
                fomega.z = 0;
                fvel.y = 0;
            }
            //-Compute fcenter / Calculo de fcenter.
            tdouble3 fcenter = fobj.center;
            fcenter.x += dt * fvel.x;
            fcenter.y += dt * fvel.y;
            fcenter.z += dt * fvel.z;
            //-Compute fvel / Calculo de fvel.
            fvel.x = float(dt * face.x + fvel.x);
            fvel.y = float(dt * face.y + fvel.y);
            fvel.z = float(dt * face.z + fvel.z);

            //-Updates floating particles.
            const float fradius = fobj.radius;
            const unsigned fpini = fobj.begin - CaseNpb;
            const unsigned fpfin = fpini + fobj.count;
            for (unsigned fp = fpini; fp < fpfin; fp++) {
                const int p = FtRidp[fp];
                if (p != UINT_MAX) {
                    tfloat4 *velrhop = Velrhopc + p;
                    //-Compute and record position displacement / Calcula y graba desplazamiento de posicion.
                    const double dx = dt * double(velrhop->x);
                    const double dy = dt * double(velrhop->y);
                    const double dz = dt * double(velrhop->z);
                    UpdatePos(Posc[p], dx, dy, dz, false, p, Posc, Dcellc, Codec);
                    //-Compute and record new velocity / Calcula y graba nueva velocidad.
                    tfloat3 dist = (PeriActive ? FtPeriodicDist(Posc[p], fcenter, fradius) : ToTFloat3(
                            Posc[p] - fcenter));
                    velrhop->x = fvel.x + (fomega.y * dist.z - fomega.z * dist.y);
                    velrhop->y = fvel.y + (fomega.z * dist.x - fomega.x * dist.z);
                    velrhop->z = fvel.z + (fomega.x * dist.y - fomega.y * dist.x);
                }
            }

            //-Stores floating data.
            if (!predictor) {
                const tdouble3 centerold = FtObjs[cf].center;
                FtObjs[cf].center = (PeriActive ? UpdatePeriodicPos(fcenter) : fcenter);
                FtObjs[cf].fvel = fvel;
                FtObjs[cf].fomega = fomega;
            }
        }
        TmcStop(Timers, TMC_SuFloating);
    }
}

/*
 * @desc 开始运行模拟计算
 * @params appname, cfg(配置对象的引用), log(log 对象)
 */
void JSphCpuSingle::Run(std::string appname, JCfgRun *cfg, JLog2 *log) {
    const char *met = "Run";
    if (!cfg || !log) return;
    AppName = appname;
    Log = log;

    // 创建计时器来测量时间间隔
    TmcCreation(Timers, cfg->SvTimers);
    // 开始运行计时器
    TmcStart(Timers, TMC_Init);

    // 载入参数
    LoadConfig(cfg);

    // 载入粒子
    LoadCaseParticles();
    ConfigConstants(Simulate2D);
    ConfigDomain();
    ConfigRunMode(cfg);

    // 初始化执行变量
    InitRun();
    UpdateMaxValues();
    PrintAllocMemory(GetAllocMemoryCpu());
    SaveData();
    TmcResetValues(Timers);
    TmcStop(Timers, TMC_Init);
    PartNstep = -1;
    Part++;

    // 主循环
    bool partoutstop = false;
    TimerSim.Start();
    TimerPart.Start();
    Log->Print(string("\n[Initialising simulation (") + RunCode + ")  " + fun::GetDateTime() + "]");
    PrintHeadPart();
    while (TimeStep < TimeMax) {
        if (ViscoTime) Visco = ViscoTime->GetVisco(float(TimeStep));
        double stepdt = ComputeStep();
        if (PartDtMin > stepdt) PartDtMin = stepdt;
        if (PartDtMax < stepdt) PartDtMax = stepdt;
        if (CaseNmoving) RunMotion(stepdt);
        RunCellDivide(true);
        TimeStep += stepdt;
        partoutstop = (Np < NpMinimum || !Np);
        if (TimeStep >= TimePartNext || partoutstop) {
            if (partoutstop) {
                Log->Print("\n**** Particles OUT limit reached...\n");
                TimeMax = TimeStep;
            }
            SaveData();
            Part++;
            PartNstep = Nstep;
            TimeStepM1 = TimeStep;
            TimePartNext = TimeOut->GetNextTime(TimeStep);
            TimerPart.Start();
        }
        UpdateMaxValues();
        Nstep++;
        //if(Nstep>=3) break;
    }
    TimerSim.Stop();
    TimerTot.Stop();

    // 结束模拟计算
    FinishRun(partoutstop);
}

/*
 * @desc 生成输出文件
 */
void JSphCpuSingle::SaveData() {
    const bool save = (SvData != SDAT_None && SvData != SDAT_Info);
    // 保持周期性粒子 如果存在
    const unsigned npsave = Np - NpbPer - NpfPer;
    TmcStart(Timers, TMC_SuSavePart);
    // 按原始顺序收集粒子值
    unsigned *idp = NULL;
    tdouble3 *pos = NULL;
    tfloat3 *vel = NULL;
    float *rhop = NULL;
    if (save) {
        // 分配内存并收集粒子数据
        idp = ArraysCpu->ReserveUint();
        pos = ArraysCpu->ReserveDouble3();
        vel = ArraysCpu->ReserveFloat3();
        rhop = ArraysCpu->ReserveFloat();
        unsigned npnormal = GetParticlesData(Np, 0, true, PeriActive != 0, idp, pos, vel, rhop, NULL);
        if (npnormal != npsave) RunException("SaveData", "The number of particles is invalid.");
    }
    // 收集额外的信息
    StInfoPartPlus infoplus;
    memset(&infoplus, 0, sizeof(StInfoPartPlus));
    if (SvData & SDAT_Info) {
        infoplus.nct = CellDivSingle->GetNct();
        infoplus.npbin = NpbOk;
        infoplus.npbout = Npb - NpbOk;
        infoplus.npf = Np - Npb;
        infoplus.npbper = NpbPer;
        infoplus.npfper = NpfPer;
        infoplus.memorycpualloc = this->GetAllocMemoryCpu();
        infoplus.gpudata = false;
        TimerSim.Stop();
        infoplus.timesim = TimerSim.GetElapsedTimeD() / 1000.;
    }
    // 记录粒子值
    const tdouble3 vdom[2] = {
            OrderDecode(CellDivSingle->GetDomainLimits(true)),
            OrderDecode(CellDivSingle->GetDomainLimits(false))
    };
    JSph::SaveData(npsave, idp, pos, vel, rhop, 1, vdom, &infoplus);
    // 释放用于粒子数据的内存
    ArraysCpu->Free(idp);
    ArraysCpu->Free(pos);
    ArraysCpu->Free(vel);
    ArraysCpu->Free(rhop);
    TmcStop(Timers, TMC_SuSavePart);
}

/*
 * @desc 模拟计算完成, 打印总览信息
 */
void JSphCpuSingle::FinishRun(bool stop) {
    float tsim = TimerSim.GetElapsedTimeF() / 1000.f, ttot = TimerTot.GetElapsedTimeF() / 1000.f;
    JSph::ShowResume(stop, tsim, ttot, true, "");
    string hinfo = ";RunMode", dinfo = string(";") + RunMode;
    if (SvTimers) {
        ShowTimers();
        GetTimersInfo(hinfo, dinfo);
    }
    Log->Print(" ");
    if (SvRes)SaveRes(tsim, ttot, hinfo, dinfo);
}
