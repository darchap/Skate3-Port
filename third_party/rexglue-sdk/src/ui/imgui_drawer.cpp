/**
 ******************************************************************************
 * Xenia : Xbox 360 Emulator Research Project                                 *
 ******************************************************************************
 * Copyright 2022 Ben Vanik. All rights reserved.                             *
 * Released under the BSD license - see LICENSE in the root for more details. *
 ******************************************************************************
 *
 * @modified    Tom Clay, 2026 - Adapted for ReXGlue runtime
 */

#include <algorithm>
#include <cfloat>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <map>
#include <string>
#include <vector>

#include <rex/assert.h>
#include <rex/chrono/clock.h>
#include <rex/logging.h>
#include <rex/ui/fonts_inter.h>
#include <rex/ui/imgui_dialog.h>
#include <rex/ui/imgui_drawer.h>
#include <rex/ui/ui_event.h>
#include <rex/ui/window.h>
#include <rex/ui/windowed_app_context.h>

#include <imgui.h>
#include <misc/freetype/imgui_freetype.h>

namespace rex {
namespace ui {

// File: 'ProggyTiny.ttf' (35656 bytes)
// Exported using binary_to_compressed_c.cpp
const char kProggyTinyCompressedDataBase85[10950 + 1] =
    R"(7])#######LJg=:'/###[),##/l:$#Q6>##5[n42<Vh8H4,>>#/e>11NNV=Bv(*:.F?uu#(gRU.o0XGH`$vhLG1hxt9?W`#,5LsCm<]vf.r$<$u7k;hb';9C'mm?]XmKVeU2cD4Eo3R/[WB]b(MC;$jPfY.;h^`ItLw6Lh2TlS+f-s$o6Q<BaRTQrU.xfLq$N;$0iR/G0VCf_cW2p/W*q?-qmnUCLYgR`*1mTi+7.nT@C=GH?a9wps_2IH,.TQg1)Q-GL(lf(T(ofL:%SS%MS=C#jfQ$X7V$t'X#(v#Y9w0#2D$CI]V3N0PRAV3#&5>#X14,MZ[Z##UE31#J&###Q-F%b>-nw'w++GM-]u)Nx0#,M[LH>#Zsvx+6O_^#l(FS7f`C_&E?g'&kcg-6Y,/;M#@2G`Bf%=(`5LF%fv$8#,+[0#veg>$EB&sQSSDgEKnIS7EM9>Z,KO_/78pQWqJE#$nt-4$F&###E`J&#uU'B#*9D6N;@;=-:U>hL&Y5<-%A9;-Y+Z&P^)9<QYN8VQM#S/Mx?c(NdBxfMKpCEPX;*qM$Q?##&5>##._L&#awnk+ib*.-Z06X1>LcA#'rB#$o4ve6)fbA#kt72LN.72L=CG&#*iX&#90Wt(F,>>#_03:)`(@2L@^x4Sj2B@PN#[xO8QkJNRR()N@#f.Mr#)t-L5FGMm8#&#/2TkLi3n##-/5##MwQ0#EB^1v&ols-)-mTMQ@-##qlQ08*lkA#aNRV7KRYML%4_s[kNa=_0Z%7Nd4[3#S@1g_/v`W#'`Fm#<MOe#_=:n#Lx;%$b(w,$g&J1$N9>B$(Q#n$oqvc$&Svv$`,TM%,PS=%OeJE%s+]l%A=Fe%']K#&7aW5&O-Nd&q&>^&GZs1'w.bA'c>u>'B-1R'%gJ.(t1tx'_jH4(iNdc(GJ*X(l`uf(^Wqr(-=Jx(=[%5)')Gb)$1vV)57Vk),8n<*BYl/*qs%]*OI5R*Fkgb*H<+q*TQv(+Xak6+?C@H+5SaT+o2VhLKd)k+i$xl+4YW=,sJd,,C*oT,Eb:K,mSPgLsF8e,Z$=rJ[<5J`E:E&#k&bV7uVco]JfaJ2M'8L#xArJ27FJx?Zgt%uov/vZ@?Gj:Kl;,jo%*K;AL7L#7G'3/J(*t.5xO+/0r+N/%ipJ/Bq_k/A>4Y/^iwl/%K:K0[HW=04D'N0wQq_00Kjt0]NJ21?p?d1T:=Y1e*&i1HLr@28x*:29A[L2Mpd%3pFIp2igO+3aXRX3M#PN3uY$d37p2=4c,s54.3SI4v0iw4JqN65G$S*5rh<65ld7E5.IRt5.f-16A/U(6IoFR6Nj7I6Y3i[6>s#s6EF=P90>=W6-Mc##=(V$#MXI%#^3=&#nd0'#(?$(#8pm(#HJa)#X%T*#iUG+##1;,#3b.-#C<x-#Smk.#GdrI3TCR/$3Ds9)?^k-$&pG/?Hn.1#rPr.LR;NHZYu-A-muPG`uqJfLK_v>#$i0B#'2[0#s6aW-AS*wp1W,/$-pZw'%]#AOC+[]O>X`=-9_cHMN8r&MsKH##77N/)8r_G3=^x]O].[]-/(pI$^=Kn<00k-$t`%/LDK5x76,G&#$or>I?v+sQ;koJM>,CS-14,dM,Hv<-cLH?01FQ*NGx0='H9V&#;Rov$8ooX_i7d;)]]>R*sVi.Lt3NM-$@dXM:uSGMDn%>-30[b's6Ct_.39I$3#bo7;FP&#YKh9&#d)KE$tok&L1tY-sTf2LP]K<Lsjr>&s9L]u-c4Au9*A>-<'3UN-PZL-NIV+85p0eZ3:.Q8bj1S*(h)Z$lel,MX_CH-.Nck-(veHZwdJe$ej+_frio0cKB$HFtRZ>#DiaWqFq7Q84okA#tiUi'Qumo%<]Xl8As(?@iLT[%tDn8gsDGA#hDu-$+HM3X_?@_8:N+q7v3G&#a7>0H3=t-?ZKm.HK+U58E/.`AcQV,tUd+Z-$fQ-Haotl8Zx2Fn)&UQ8c6E&docd.%&^R]u)x:p.N*wIL8+fsrk+5<MR@v58X^?xKxUi^6A``6MU-lPSgJ$##P*w,v%,[0#Rhi;-`2$I%*nhxu67Np.(AP##Y+YB]LD_K*NPG])IsiA#Dqi05siIfL;G;QMM8-##?bu&#,>###>jq:9%/v2;f`?J8fDrG%fmWw9gl'ENgjG:,EC%<-WW5x'6eaR86kf2`5alP&u]::.'a0i);c)3LN3wK#gZb19YvMa,?IggL3xoFMTK_P85<B9&NP'##mF#m8$6<QhEn>.)0xLp7gw]m_oM++.`=JfLm)1#.gGKd4N^@N%M'Np7ZO:k)VTqt%EO`gurjj;-0r%;%I<Ga>'M,W-(hdnXP4bA,%GLp75c<LYo5oMiXKh+0O>`QUWh<_&.ZoDuWmL<LKx(B6eVxZ9,V@Z76OM=-Ke??.]RXk:UD`?%^FHM&LMQY-SJmDc?1&Z$gq`gMi.(58gkcA#l5#N9#9Z;%Y*K-;8K?E5#0]guh&tP8m7:f[<f568<JtpBUNiF*4db;-[s[n&9o`Y-R7B$L4*XQ8t$,?.Vqa_&fQB?-/]2u$#JUp7S+5wp=25?R6W5@MA)jB%lpNp7^'9U)jNtKNBU0I,'XFQ/&&###'><h#I[*T.73rI3#1[m%:TUv[NC90/Q]i.L(dt_/1dC_&8QFeFKgL<L+qdU2f$;R3rftK3GiIn&ddcA,CDkGM'CYcM#c[#%(MgTTc645&L(T&#b:o<<l/tDYp$M3<QQGb@vjfe$i@nEI?ZKal44)=-T4sP-u0@q$:-d9`EQjDNuagC-_1X_$PQ`&#g1iJs&h'a)J`C<-M`B-'sB1tL>CVJ+7:P&#Wj7n$+8sb<:+R.Qx7m<-T`&0%3TK<-h.oN'eSYW-g7D^6mu<W)>7Rc;:cIH%5hWHX9uCq'RC/2'GZZ(=:.$ekS>k((WP_=-,8dT%;]DeHjNJ'HOsgj-vUa$UFQO68Ic+k2HwQ'(0Kgn8V=:</jUcP(Nir;JdYO&/+mZe;Cmw@^[x8IH2i<w-u$Hq7lB)KE@V)7)'R4tQc*Fv%0DTgLvgjIL%Xi.Lb+pE='Rf3M_o>*(iM]?[]-#9)#tb,;mdUw.SB+-8M*cj)1)A@;:O#kipW<78t=vERat3KN(RZ&%0)3XJh/1q+<E3'mJ9?m'as868qukA#>'_5'r1GX`4;kNbkh&@-HCp[+c+Z68=Z9:BM#Jn$R+0Au6A)K:YXr1d^8ILE65V'#Y_%n8Mc`3r:>H9%PMhj9GVCh3F3wm81EG&#,`**<3AEYLN1pA#>q0p&(^?@'Bl+&>klY'vO%co7juS^d)a#9%=&m9.m`0i)rQNm8*GF]uI9+W-wmw5_L07xLC8qT;`9%90i^Gx'abQp7)>5wLq3n0#/U0V8+B^G3%3,0:3w<W8.p?#+Hp]p7*MXK3JhpgLE-q8P&(mW-dxr>R]4fJ(d%2N9Z-r_&7rjQS<XpW&&A-W--@;qi&29tUa5N&#gB)gL8,Ap-elVKW5TwR*0l;wPAjbS(()Eb<iU(Y&3:8,($2)49;(fvnOpTx=Jx#6qqjL<LA?*l2PS&i;d2W&HBfj.L6$[J8b(%df[2Q:8bDew'2N#k9gbY2VAHX*2L0,rBcSWf%AZbqKY9g8)k4ZQ_8dP^0#$$3:,hwW&^?Ie-:Z&Eu:RL>,sM;n9g#>ve^2SK)71JTRxD)o0@1wWA2#E;<PRZ;%>xov>0f^-QQQYVBeT+?-7kMD5d0B#QZAW0:Z<A^HCkC&Oe4LI89xAs997Um.Xi,FNt-iE)6=nm8<>jUK[OZi&61L&#>CCj;r]/RLH'(j>+$P-R9bF69`%f@[p-JZ*.hnp7;-ge$NSi?-qx8;-V<ZA+1q8N9tGRWAv9j+(7=DHFC=[Z:UVgY(=5)N<)b)OBsUeA,RgI#P76ZE,3tQnTSSff&N,76LMX[r;%'1'#(AP##r-7G;4akA[ve^@%sVi.LS_r&&;4qgL>]Z##,B?nrCn,)'(Q%a-sI^W&9'i&#SrRfL`Zwe%k.jA,xf:-%<Gf&#_:JfL:JQi*c/Z)<'7(a6g/mx'aPc689TO]uo<MU'5+WZPi.cE<g(_>$+:t>-v^)'%of?pg=`N_*o'w<LJb*=-q`6]'Fh0BI@9[&%7bI4VM$D*'C[:RaFCI<-v=B[%7hep7=wRLa#E-v.K#gmA.2(LNqLC2)bqDp7.5HZAm;&_8ekx;8FmR/&mTV:@#CTp9:td>)3(ip7XqF]uN-Fj9l=K/+sAH^*I=5qBCRt-,T163BO%ov7%,sb&T=XaZ$(#GM0#Qp%a]Cs7HNbxum=g@>wb%?7N:Fk'0PYRhUv-tLWr+P(lLM/:9N*H=KRZT'Pf2;.@2<)#pVl1MwLk0&;tUAuP3w.Le.]T/*Mc##O->>#9NCU.73rI3ZbA;%^xT3BS2L#$uLjf%53Kt-2SJMBFZ.m0cmcPS)aX%(c]Yg<^[G6;$W(8*2&$X->B+kk^$D'8E@P&#I-nT'u5pm8u;Be=AJ8F-T6po)A:&?-CPcd$rDtJjLUsv'7Hx_onecgHu78k:D#]4;tb)$-UHAm8h;2c>8J<@.(W=p&oVoY?&@+w7-)ri'bb=+<b2:*S]stQ_=5>X:<Q(Ka=4)=-+'h&,:TKs-#>#29.*DW/tNqT&QAl29xj+AuD:*+lnW]D,3l6<-PX9YYw)vX&=WuT8H=AbIs[`Am2xcW-jqbn*cZV%_t/Z&QpvGJ(i2.^==iWDurfn:Ml;-##/-U%)x$+1:lROdt*mpM=i4/)Zdr'H'P[N>-EKHl$hUvf:P'Q3`u*IM&uZA39^0F[pUB+n8hq+?I`L'-)2>Cq71g/6(?(oR&iBRiLr7w;-[HuL3u6e2&V:QjBJ:9iuF8.a<ARjp'0Abr&l&:P9L3B:^7aj(8PK:(QkKLTMsCt?'Yqkd4'DW-2%^Bq7xR[[%i_Nh11uZp7LW^G3(Y0(=DRYF%#jl<-h&*u$;S^,Mw?<K**]I^FG,614Dd@N%$;Ed;0pkKNJl:a%rL@Y-&5n0#TD,##%5w.LCn^-)uH90:H;lA#;qp1(J7rLExpE]F=%,(HFI8V%095g)3fBemf@#kGO5###'5>##PHT<-.4r1&,qBE<es9B#LG'>#LK,W-fIO4kX@%%#tUB#$57>uHN^KeX'-cD)d.s*<i5qHQhe%D<KIF&#_UK]u**<gLY7<C0t#jgLqWQ>#P<Eh#$`b3(.hFQ/0rC$#P9cY#]TJLC,=5(H)L2Y&SE^=-6hk.?lG8<9c6Bt+=B`&#Ee<wTFMGT&2P###3XG$>2+c&#ok:/:3l(RsPD###2d#<-%,.t$5@HgL/mu<+PXhv[Bgb4)GO;eMZQMr?,tXvIIe;t-P2l?G2j1v^)3l$'mEa68K1l@7.`V[GG#)C]Y&f;]?OM>&x]i.L(/5##BO+k9Xp0B#NS9>#+7E<-d]nl$Yw6v9YK*6(sxGug]oko$_'l_-Ai%RMq<&_8o2@2L@@AS)c8(<-c&r]$J9oq7g?(m9LIS;H-)KfF@qVI*^ACO9fKc'&6k/q7RD&Fe&*l2LSQUV$vC#W-lwf&v:'n]%D4xVH4&(^#0jg<-@r'29EQT_6Gx)Q&':nKC>s6.6*;X^ZH.->#>atJC6`hJ>NjO?-^5l-8Tj72LFIRp7,:<Q3$)F/)5Wu>-.wruM;q0)(YHWp7@i('#'2,##8ZDK<cUb5A@]kA#9fRjNh6]0#&P[g)P`0i)0d](%^m[v$q)TVHKS9.v<%SR8<<9*#2<_C-7)hg'Orqs-QEsFMp=h19vO%o<M5,##n[li98-aQL[3=G%6J>M9dvR3kYKd&#V(f+MR?xK#liDE<[/RM3M7-##1na_$+q)'%xheG*DsXbN^BxK#R90%'vrIfL.r&/LT*=(&A's2;O56>>/jsX_Z_+/(NSi.L>jEG']iNUR238^&?MR49ne<Gm'IVQ(((wQ8*9Xd=.H6h'lv049,uPGG:Jw_-Oghr?PvblrD)TJGlq=42p_N^-e59I$%]4d<r%wd+^1Iu'n94395H72LcojA#^QWp7i0/kXEFEm8TjjM3j^an8JUbA#FS0a=l6G]uVMHW-P-t2DpAqa)1xF&#l.HB,)'sFl=TA_%6DPT^$ok%(vc<'#Z'[0#SHW-$2W(1:C,QD.Ln5%()ocjK+uZH+o[2n<rh/:)jPHmk0S_GDMgTd4o'1c&X(ek90sTn`pij7'7_j:HY/6ElAGdvDUpK#$mgJH+.`]&=*6J_60lSc&>A>.M5N=h#=eq:T?k)/:;SF&#;k-gLXL0e,e6JdDNHj?@ihvi&wNT-;6`E.FAl):%3WK<-:EI+'^([:.+lQS%?_,c<8L[W&T7-##`QHXA=(xn&Yqbf(kg<LWVZx2&&]?68vN6pgnUIu'M6YE<i(72Lgs00:xi8u_)F.^'J5YY#)PjfL=w;/:ilAqBqSBt7r[b&#hEqhLJLvu#DO*e*),8Z>e2-g)[OksHT*bm+Do5IM<_jILK4Pv'=u5a*E#Z^FHmn),Dheq.+Sl##04kWoAl[W]rYHI%+d@a-.dm<%#1[,MtRBt)O(35&>-f;-J=<n)/on,48,Ut':/;3t&u5*P`^0d%5P'gLvk,6+)>.g:(xp/)U]W]+RCwgCbE0#Aes-h%vr_BF0;=K34Yv',/GoEYQQ-U&5&Aw>]ewGt?k,l$1oR8VCkF<%+nTH4Z8f%/M&dQ/(2###S`%/L*cS5JX&V_$iac=(LG:;$ZcRPA.$+bHU7-###c7^O1[qS%)S#qT=lI(#=,o_Q.^r#(w1I<-+PK`&,o'^#GhsWQt6j(,]_<g:%t)qK.h`u&QggR8p0S`ABAla*GK[,MtDNg8IUL^#'5>##IN]p7i4mFuXih@-t=58.1&>uu$&h2`2H:%'T):wPGJuD%5DJTIUXbA#pvRdM=WcO-uhKj%0ej?Ppr<A+6o*KWQJ,x)lE59)+HL$Gs2J?n(Y]9@3*?$(x%Id;IZO]un4WJ%ncg;-3Fd<-;Q*((VD,C'q3n0#J,,##Jw@D<aNO&#PYkb$S(Fk4:Ru&#()>uu9k,h%cwfi'B.x`=Tg^d4%45GM@iZQ'YGHP9(MGGG<CD?@lb`^)j()<-X,r]$5rJ&voRl5(4@m=&l#I]uFX9b<,#hdOsBqA.&?sI3w+K?I(kEe$PLRN9De:'#6]###QfB##:a;l9Z^:m9;pd,k#en3)wXN*<W.r_&Y]$O9+]^8iC,`l--hN,m%VO;&@RG&#]*J788ix?7HZU18T0Y(f;F@q7O51Llmr1hYQw<kkDqkjDx&AKu$-a&#2f<p^oXAZ$`([K:&tF?.T;a38igw-+BB]c;N)man%)(gL$V0`$ilcp7Clp/NCP,3)Ev$&41Nw`*@0.9@iN7rqS]Ll)H)1W-]3n0#`6%_8I;TGlGR1H-w,TRLb8jn'+.UE,fK^n&SO7m83wgDlG=](WrCeA%ioD(=;+<r7r)`0G8MT&#Dfdf.>vbG3jZJp7FAeEP&e<tDI$f[@Kn,$%6M.M:12f4V.,j80DmpKC;'+-tl;rn-okgdkq5%Y%pBmp@r,g2`PR-N9&<D^63_RR(L<*b*bP5<-s>PL'8k8k91/D?[-7(&(m@?q7kdH)cDfYN)@9TDSe;DG)uQh&#k+'p74N)^ohl=,'';[P9_kisBjgU,&g>Ok2=4'K%cl@Nii)3q-_.1U9,.QL/2&>uuF*^TVA7Bs&;W36AZ(j'muJG4M_<bc%_Q?6']Td`*<g-[-%u,N<Tcm(F,rGF>CpO4)0kNeFKG2V?'jkgNvkK<&MQU+<[xKVaY>/T@&Jp'HtA$a&5U&R8bs:RWYiYeQu4k(NgxE$%X6V&#X+3O-u_dQ8/_-ldRf1W-2dpGe*E^r7d>S^JisoC%s`^68r*d;))C[p7W?<McU=n%P4'Ho$8VG29mZvQ:H1[^&foZaE#jbxu$lZp7%2s&-9rJfL4s*C8mlB#$P:=QSF-*j$[A'aNtobb.Z'[0#kQ*n%:i4%(JN#,2#9bp7q>[6OfId2P&;Hr7cpB#$X8-l93rg996Nb4):v0<-Y7`[eEdoW*l/xNN9<&v,%nra*-?078.F8o8aP+Au]ZX2L:1Bn*fuW1;N&&3M5U#x'<w:u??w]RjZCNv'[c)BFnoDbPf][`*(pBQ8mcN)YW/b,'^Y(<-QIIZ*eRrP8r=24D<.#L%vXMnNG_`78f:HO*m$N$-PMR8`[9jb8tBuTW.WqG`++ho%pI<#6EZA`X;)&G;EY+Aus#XjBZXG&#j[*RBY<-El+AI':)Z.=`]4i78Z]]2`=R$m8,@^2`Qs849_LfH[(X+`)X0-2(a+@>-KA$ZAf0YS&_;AL;>g6pgV==5A6R.db1Wbs'MC9-)5u:ENe7-##H:O&=$^]712c&gL,%,cM+4(5SmwUF<qhvhO+X[b+rh(hL5:Tf$^gs]OK?Qd&kJ1>-x-*j0c-G(-9Y`N*$0_k2ece`*#JdQ8Fk=9W'&%kCjYeaHC@nL-$[d;B*<^#DuHQNYF#>Q8L*fW]8cJX/,CWR83+pVooXXk'1HCL:6I%T(`2VY(An&R8?uN]'WLJM'$/*JLXm@8JhS](6l0tv'x06oDXFC'%=7CP8fCKetbqp0%;=0iC/kRkrjK5x'qtD_Or/9x:VWF&#<r)<-rHR.'>LW`<04=q0E8/c&hItt'a8J0)kY#Q8nnV78o=6UT`-1t%-FuN:xA1J[Oq`p77%72L9$2<?vo]Q(S/,`%VaGY>-'[qp)mH^*[eL]u/o(HF0iu.-vwoW_wn=O:[3NPcDh)Zn)ex[T%LaP-VC%f$36t1CvSw,X>^>Z4q=Z58]evqT5.xP33h839>So>InZb%=w=>F%$Mdm_FjSEEwGJMN3B,-(b@mHM;uFr$r&V@-Vp;m$bF6XJbM0-'-PgQJ=Z.lBE?=x>YBPX9N3?&+0)xm'wQsH$K]MP9cmv9glREr(>=n-k;/6t$r]2@-Ips&d-8oS@pb5r@lwcQ:aum))u=KkrVv[n>Lu,@RvlOE.^Puk;v4[+9.2A2LrPn'&]/?pg&.Rq$9-vc6BUpD*8[?:BmMq*9.HFt_QSl##O->>#b7278#r%34A$;M%+=hlTsVPp'X8N&Zu/To%mDh:.,umo%5VIl90wn5F9;_OFJ?=?JbjcX($^)Rj2vao7W9Udkr[F%8:@(4F@5W5_oHOG%M4Y@G:P+JGUsRA%UeO-;Tr+OOHi8i:F$aC=K@82L(__3:>H-g)S65e;B@:xnT_x0+x,2N:rmL4)VtH#)NF7WAs,Zx'uQpE<NJEaGq^'%'j%gpB;Je(-/`%=-8`&.6X/4S-FK=f'F>U78_TX=?1s?cZYlBd'<IaN9E=Ws^iqV_,Yei68%U@9KA-Rb'2WK78hIZ;%DkE2LDfvd(M%Jn&KSC<-mSZ[$ca<@9#`'^#nx(X-BLpU@YmB#$0Q?d8/4hFco+Eu$fY%F<]%*?@FBA,;vV@-Fo:Cu047V2B18,'$Rqmr*$J4gU<7(p(Y5:wPn;v&'C(^('$9#v/1<#e+K2ta*SV0<ISF0'HPQB%oF'7F'IZ'N9$/+8Vf[VC2)&4V&7rpgL<=XD+`2aO;_((e*FKK=-J.fQ-]HGM.IhF(=2tJQ(C9ES.qL)*NpYd.:b[+Au-g([I%QL@-cVfJ8D>BugDAVB-vlc_fV5gc*s&Y9.;25##F7,W.P'OC&aTZ`*65m_&WRJM'vGl_&==(S*2)7`&27@U1G^4?-:_`=-+()t-c'ChLGF%q.0l:$#:T__&Pi68%0xi_&Zh+/(77j_&JWoF.V735&S)[R*:xFR*K5>>#`bW-?4Ne_&6Ne_&6Ne_&lM4;-xCJcM6X;uM6X;uM(.a..^2TkL%oR(#;u.T%eAr%4tJ8&><1=GHZ_+m9/#H1F^R#SC#*N=BA9(D?v[UiFk-c/>tBc/>`9IL2a)Ph#WL](#O:Jr1Btu+#TH4.#a5C/#vS3rL<1^NMowY.##t6qLw`5oL_R#.#2HwqLUwXrLp/w+#ALx>-1xRu-'*IqL@KCsLB@]qL]cYs-dpao7Om#K)l?1?%;LuDNH@H>#/X-TI(;P>#,Gc>#0Su>#4`1?#8lC?#<xU?#@.i?#D:%@#HF7@#LRI@#P_[@#Tkn@#Xw*A#]-=A#a9OA#d<F&#*;G##.GY##2Sl##6`($#:l:$#>xL$#B.`$#F:r$#JF.%#NR@%#R_R%#Vke%#Zww%#_-4&#1TR-&Mglr-k'MS.o?.5/sWel/wpEM0%3'/1)K^f1-d>G21&v(35>V`39V7A4=onx4A1OY5EI0;6Ibgr6M$HS7Q<)58UT`l8Ym@M9^/x.:bGXf:f`9G;jxp(<n:Q`<rR2A=vkix=$.JY>(F+;?,_br?0wBS@49$5A8QZlAQ#]V-kw:8.o9ro.sQRP/wj320%-ki0)EKJ1-^,,21vcc258DD39P%&4=i[]4A+=>5ECtu5I[TV6Mt587Q6mo7tB'DW-fJcMxUq4S=Gj(N=eC]OkKu=Yc/;ip3#T(j:6s7R`?U+rH#5PSpL7]bIFtIqmW:YYdQqFrhod(WEH1VdDMSrZ>vViBn_t.CTp;JCbMMrdku.Sek+f4ft(XfCsOFlfOuo7[&+T.q6j<fh#+$JhxUwOoErf%OLoOcDQ@h%FSL-AF3HJ]FZndxF_6auGcH&;Hggx7I1$BSIm/YoIrVq1KXpa._D1SiKx%n.L<U=lox/Ff_)(:oDkarTCu:.T2B-5CPgW=CPh^FCPidOCPjjXCPkpbCPlvkCPm&uCPn,(DP@t>HPA$HHPB*QHPC0ZHPD6dHPD3Q-P_aQL2<j9xpG';xpG';xpG';xpG';xpG';xpG';xpG';xpG';xpG';xpG';xpG';xpG';xpG';xpCUi'%jseUCF3K29]cP.PK)uCPK)uCPK)uCPK)uCPK)uCPK)uCPK)uCPK)uCPK)uCPK)uCPK)uCPK)uCPK)uCPK)uCPT$au7ggUA5o,^<-O<eT-O<eT-O<eT-O<eT-O<eT-O<eT-O<eT-O<eT-O<eT-O<eT-O<eT-O<eT-O<eT-RWaQ.nW&##]9Pwf+($##)";

static_assert(sizeof(ImmediateVertex) == sizeof(ImDrawVert), "Vertex types must match");

ImGuiDrawer::ImGuiDrawer(rex::ui::Window* window, size_t z_order, FontSetupCallback font_setup)
    : window_(window), z_order_(z_order), font_setup_(std::move(font_setup)) {
  Initialize();
}

ImGuiDrawer::~ImGuiDrawer() {
  SetPresenter(nullptr);
  if (!dialogs_.empty()) {
    window_->RemoveInputListener(this);
    if (touch_pointer_id_ == TouchEvent::kPointerIDNone && mouse_buttons_down_) {
      window_->ReleaseMouse();
    }
    if (text_input_active_) {
      window_->SetTextInputActive(false);
    }
  }
  if (internal_state_) {
    ImGui::DestroyContext(internal_state_);
    internal_state_ = nullptr;
  }
}

void ImGuiDrawer::AddDialog(ImGuiDialog* dialog) {
  assert_not_null(dialog);
  // Dialogs may be constructed on kernel/guest threads (XamShow*UI dispatch);
  // the dialog list, the window input listener list and the presenter UI
  // drawer list are all UI-thread state, so marshal to the UI thread. If the
  // UI loop is already gone (shutdown), skip - the dialog will never be drawn
  // and the dispatch path deletes it.
  WindowedAppContext& app_context = window_->app_context();
  if (!app_context.IsInUIThread()) {
    app_context.CallInUIThreadSynchronous([this, dialog]() { AddDialogImpl(dialog); });
    return;
  }
  AddDialogImpl(dialog);
}

void ImGuiDrawer::AddDialogImpl(ImGuiDialog* dialog) {
  // Check if already added.
  if (std::find(dialogs_.cbegin(), dialogs_.cend(), dialog) != dialogs_.cend()) {
    return;
  }
  if (dialogs_.empty() && !IsDrawingDialogs()) {
    // First dialog added. !IsDrawingDialogs() is also checked because in a
    // situation of removing the only dialog, then adding a dialog, from within
    // a dialog's Draw function, re-registering the ImGuiDrawer may result in
    // ImGui being drawn multiple times in the current frame.
    window_->AddInputListener(this, z_order_);
    if (presenter_) {
      presenter_->AddUIDrawerFromUIThread(this, z_order_);
    }
  }
  dialogs_.push_back(dialog);
}

void ImGuiDrawer::RemoveDialog(ImGuiDialog* dialog) {
  assert_not_null(dialog);
  WindowedAppContext& app_context = window_->app_context();
  if (!app_context.IsInUIThread()) {
    app_context.CallInUIThreadSynchronous([this, dialog]() { RemoveDialogImpl(dialog); });
    return;
  }
  RemoveDialogImpl(dialog);
}

void ImGuiDrawer::RemoveDialogImpl(ImGuiDialog* dialog) {
  auto it = std::find(dialogs_.cbegin(), dialogs_.cend(), dialog);
  if (it == dialogs_.cend()) {
    return;
  }
  if (IsDrawingDialogs()) {
    // Actualize the next dialog index after the erasure from the vector.
    size_t existing_index = size_t(std::distance(dialogs_.cbegin(), it));
    if (dialog_loop_next_index_ > existing_index) {
      --dialog_loop_next_index_;
    }
  }
  dialogs_.erase(it);
  DetachIfLastDialogRemoved();
}

void ImGuiDrawer::Initialize() {
  // Setup ImGui internal state.
  // This will give us state we can swap to the ImGui globals when in use.
  internal_state_ = ImGui::CreateContext();
  ImGui::SetCurrentContext(internal_state_);

  auto& io = ImGui::GetIO();

  // TODO(gibbed): disable imgui.ini saving for now,
  // imgui assumes paths are char* so we can't throw a good path at it on
  // Windows.
  io.IniFilename = nullptr;

  SetupFonts();

  // Dialogs push runtime font sizes (PushFont(font, 18..40)); the legacy
  // prebaked atlas can only bitmap-scale its 10px bake to serve them, which
  // blurs. With this flag glyphs rasterize on demand at the drawn size, and
  // io.DisplayFramebufferScale (set per-frame in Draw on macOS) additionally
  // rasterizes them at Retina display pixel density while layout stays
  // logical. The texture create/update requests are serviced by
  // ProcessImGuiTextureRequests through the platform-agnostic ImmediateDrawer.
  io.BackendFlags |= ImGuiBackendFlags_RendererHasTextures;

  auto& style = ImGui::GetStyle();
  style.ScrollbarRounding = 0;
  style.WindowRounding = 0;
  style.TabRounding = 0;
  style.Colors[ImGuiCol_Text] = ImVec4(0.89f, 0.90f, 0.90f, 1.00f);
  style.Colors[ImGuiCol_TextDisabled] = ImVec4(0.60f, 0.60f, 0.60f, 1.00f);
  style.Colors[ImGuiCol_WindowBg] = ImVec4(0.00f, 0.06f, 0.00f, 1.00f);
  style.Colors[ImGuiCol_ChildBg] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
  style.Colors[ImGuiCol_Border] = ImVec4(0.00f, 0.35f, 0.00f, 1.00f);
  style.Colors[ImGuiCol_BorderShadow] = ImVec4(0.00f, 0.00f, 0.00f, 0.00f);
  style.Colors[ImGuiCol_FrameBg] = ImVec4(0.80f, 0.80f, 0.80f, 0.30f);
  style.Colors[ImGuiCol_FrameBgHovered] = ImVec4(0.90f, 0.80f, 0.80f, 0.40f);
  style.Colors[ImGuiCol_FrameBgActive] = ImVec4(0.90f, 0.65f, 0.65f, 0.45f);
  style.Colors[ImGuiCol_TitleBg] = ImVec4(0.00f, 0.40f, 0.00f, 1.00f);
  style.Colors[ImGuiCol_TitleBgCollapsed] = ImVec4(0.00f, 0.33f, 0.00f, 1.00f);
  style.Colors[ImGuiCol_TitleBgActive] = ImVec4(0.16f, 0.65f, 0.00f, 1.00f);
  style.Colors[ImGuiCol_MenuBarBg] = ImVec4(0.00f, 0.35f, 0.00f, 1.00f);
  style.Colors[ImGuiCol_ScrollbarBg] = ImVec4(0.00f, 0.40f, 0.11f, 0.59f);
  style.Colors[ImGuiCol_ScrollbarGrab] = ImVec4(0.00f, 0.68f, 0.00f, 0.68f);
  style.Colors[ImGuiCol_ScrollbarGrabHovered] = ImVec4(0.00f, 1.00f, 0.15f, 0.62f);
  style.Colors[ImGuiCol_ScrollbarGrabActive] = ImVec4(0.00f, 0.91f, 0.09f, 0.40f);
  style.Colors[ImGuiCol_PopupBg] = ImVec4(0.20f, 0.20f, 0.20f, 0.99f);
  style.Colors[ImGuiCol_CheckMark] = ImVec4(0.74f, 0.90f, 0.72f, 0.50f);
  style.Colors[ImGuiCol_SliderGrab] = ImVec4(1.00f, 1.00f, 1.00f, 0.30f);
  style.Colors[ImGuiCol_SliderGrabActive] = ImVec4(0.34f, 0.75f, 0.11f, 1.00f);
  style.Colors[ImGuiCol_Button] = ImVec4(0.15f, 0.56f, 0.11f, 0.60f);
  style.Colors[ImGuiCol_ButtonHovered] = ImVec4(0.19f, 0.72f, 0.09f, 1.00f);
  style.Colors[ImGuiCol_ButtonActive] = ImVec4(0.19f, 0.60f, 0.09f, 1.00f);
  style.Colors[ImGuiCol_Header] = ImVec4(0.00f, 0.40f, 0.00f, 0.71f);
  style.Colors[ImGuiCol_HeaderHovered] = ImVec4(0.00f, 0.60f, 0.26f, 0.80f);
  style.Colors[ImGuiCol_HeaderActive] = ImVec4(0.00f, 0.75f, 0.00f, 0.80f);
  style.Colors[ImGuiCol_Separator] = ImVec4(0.00f, 0.35f, 0.00f, 1.00f);
  style.Colors[ImGuiCol_SeparatorHovered] = ImVec4(0.36f, 0.89f, 0.38f, 1.00f);
  style.Colors[ImGuiCol_SeparatorActive] = ImVec4(0.13f, 0.50f, 0.11f, 1.00f);
  style.Colors[ImGuiCol_ResizeGrip] = ImVec4(1.00f, 1.00f, 1.00f, 0.30f);
  style.Colors[ImGuiCol_ResizeGripHovered] = ImVec4(1.00f, 1.00f, 1.00f, 0.60f);
  style.Colors[ImGuiCol_ResizeGripActive] = ImVec4(1.00f, 1.00f, 1.00f, 0.90f);
  style.Colors[ImGuiCol_Tab] = style.Colors[ImGuiCol_Button];
  style.Colors[ImGuiCol_TabHovered] = style.Colors[ImGuiCol_ButtonHovered];
  style.Colors[ImGuiCol_TabActive] = style.Colors[ImGuiCol_ButtonActive];
  style.Colors[ImGuiCol_TabUnfocused] = style.Colors[ImGuiCol_FrameBg];
  style.Colors[ImGuiCol_TabUnfocusedActive] = style.Colors[ImGuiCol_FrameBgHovered];
  style.Colors[ImGuiCol_PlotLines] = ImVec4(1.00f, 1.00f, 1.00f, 1.00f);
  style.Colors[ImGuiCol_PlotLinesHovered] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
  style.Colors[ImGuiCol_PlotHistogram] = ImVec4(0.90f, 0.70f, 0.00f, 1.00f);
  style.Colors[ImGuiCol_PlotHistogramHovered] = ImVec4(1.00f, 0.60f, 0.00f, 1.00f);
  style.Colors[ImGuiCol_TextSelectedBg] = ImVec4(0.00f, 1.00f, 0.00f, 0.21f);
  style.Colors[ImGuiCol_ModalWindowDimBg] = ImVec4(0.20f, 0.20f, 0.20f, 0.35f);

  frame_time_tick_frequency_ = double(rex::chrono::Clock::QueryHostTickFrequency());
  last_frame_time_ticks_ = rex::chrono::Clock::QueryHostTickCount();

  touch_pointer_id_ = TouchEvent::kPointerIDNone;
  reset_mouse_position_after_next_frame_ = false;
}

std::optional<ImGuiKey> ImGuiDrawer::VirtualKeyToImGuiKey(VirtualKey vkey) {
  static const std::map<VirtualKey, ImGuiKey> map = {
      // Navigation
      {ui::VirtualKey::kTab, ImGuiKey_Tab},
      {ui::VirtualKey::kLeft, ImGuiKey_LeftArrow},
      {ui::VirtualKey::kRight, ImGuiKey_RightArrow},
      {ui::VirtualKey::kUp, ImGuiKey_UpArrow},
      {ui::VirtualKey::kDown, ImGuiKey_DownArrow},
      {ui::VirtualKey::kHome, ImGuiKey_Home},
      {ui::VirtualKey::kEnd, ImGuiKey_End},
      {ui::VirtualKey::kPrior, ImGuiKey_PageUp},
      {ui::VirtualKey::kNext, ImGuiKey_PageDown},
      {ui::VirtualKey::kInsert, ImGuiKey_Insert},
      {ui::VirtualKey::kDelete, ImGuiKey_Delete},
      {ui::VirtualKey::kBack, ImGuiKey_Backspace},
      {ui::VirtualKey::kReturn, ImGuiKey_Enter},
      {ui::VirtualKey::kEscape, ImGuiKey_Escape},
      {ui::VirtualKey::kSpace, ImGuiKey_Space},
      // Letters
      {ui::VirtualKey::kA, ImGuiKey_A},
      {ui::VirtualKey::kB, ImGuiKey_B},
      {ui::VirtualKey::kC, ImGuiKey_C},
      {ui::VirtualKey::kD, ImGuiKey_D},
      {ui::VirtualKey::kE, ImGuiKey_E},
      {ui::VirtualKey::kF, ImGuiKey_F},
      {ui::VirtualKey::kG, ImGuiKey_G},
      {ui::VirtualKey::kH, ImGuiKey_H},
      {ui::VirtualKey::kI, ImGuiKey_I},
      {ui::VirtualKey::kJ, ImGuiKey_J},
      {ui::VirtualKey::kK, ImGuiKey_K},
      {ui::VirtualKey::kL, ImGuiKey_L},
      {ui::VirtualKey::kM, ImGuiKey_M},
      {ui::VirtualKey::kN, ImGuiKey_N},
      {ui::VirtualKey::kO, ImGuiKey_O},
      {ui::VirtualKey::kP, ImGuiKey_P},
      {ui::VirtualKey::kQ, ImGuiKey_Q},
      {ui::VirtualKey::kR, ImGuiKey_R},
      {ui::VirtualKey::kS, ImGuiKey_S},
      {ui::VirtualKey::kT, ImGuiKey_T},
      {ui::VirtualKey::kU, ImGuiKey_U},
      {ui::VirtualKey::kV, ImGuiKey_V},
      {ui::VirtualKey::kW, ImGuiKey_W},
      {ui::VirtualKey::kX, ImGuiKey_X},
      {ui::VirtualKey::kY, ImGuiKey_Y},
      {ui::VirtualKey::kZ, ImGuiKey_Z},
      // Digits
      {ui::VirtualKey::k0, ImGuiKey_0},
      {ui::VirtualKey::k1, ImGuiKey_1},
      {ui::VirtualKey::k2, ImGuiKey_2},
      {ui::VirtualKey::k3, ImGuiKey_3},
      {ui::VirtualKey::k4, ImGuiKey_4},
      {ui::VirtualKey::k5, ImGuiKey_5},
      {ui::VirtualKey::k6, ImGuiKey_6},
      {ui::VirtualKey::k7, ImGuiKey_7},
      {ui::VirtualKey::k8, ImGuiKey_8},
      {ui::VirtualKey::k9, ImGuiKey_9},
      // Function keys
      {ui::VirtualKey::kF1, ImGuiKey_F1},
      {ui::VirtualKey::kF2, ImGuiKey_F2},
      {ui::VirtualKey::kF3, ImGuiKey_F3},
      {ui::VirtualKey::kF4, ImGuiKey_F4},
      {ui::VirtualKey::kF5, ImGuiKey_F5},
      {ui::VirtualKey::kF6, ImGuiKey_F6},
      {ui::VirtualKey::kF7, ImGuiKey_F7},
      {ui::VirtualKey::kF8, ImGuiKey_F8},
      {ui::VirtualKey::kF9, ImGuiKey_F9},
      {ui::VirtualKey::kF10, ImGuiKey_F10},
      {ui::VirtualKey::kF11, ImGuiKey_F11},
      {ui::VirtualKey::kF12, ImGuiKey_F12},
      // Modifiers
      // Win32 sends the generic VKs (WM_KEYDOWN gives VK_SHIFT etc.), SDL and
      // GTK send the sided ones - both must be mapped.
      {ui::VirtualKey::kShift, ImGuiKey_LeftShift},
      {ui::VirtualKey::kControl, ImGuiKey_LeftCtrl},
      {ui::VirtualKey::kMenu, ImGuiKey_LeftAlt},
      {ui::VirtualKey::kLShift, ImGuiKey_LeftShift},
      {ui::VirtualKey::kRShift, ImGuiKey_RightShift},
      {ui::VirtualKey::kLControl, ImGuiKey_LeftCtrl},
      {ui::VirtualKey::kRControl, ImGuiKey_RightCtrl},
      {ui::VirtualKey::kLMenu, ImGuiKey_LeftAlt},
      {ui::VirtualKey::kRMenu, ImGuiKey_RightAlt},
      {ui::VirtualKey::kLWin, ImGuiKey_LeftSuper},
      {ui::VirtualKey::kRWin, ImGuiKey_RightSuper},
      {ui::VirtualKey::kCapital, ImGuiKey_CapsLock},
      {ui::VirtualKey::kNumLock, ImGuiKey_NumLock},
      {ui::VirtualKey::kScroll, ImGuiKey_ScrollLock},
      {ui::VirtualKey::kSnapshot, ImGuiKey_PrintScreen},
      {ui::VirtualKey::kPause, ImGuiKey_Pause},
      // OEM keys
      {ui::VirtualKey::kOem3, ImGuiKey_GraveAccent},
      {ui::VirtualKey::kOemMinus, ImGuiKey_Minus},
      {ui::VirtualKey::kOemPlus, ImGuiKey_Equal},
      {ui::VirtualKey::kOem4, ImGuiKey_LeftBracket},
      {ui::VirtualKey::kOem6, ImGuiKey_RightBracket},
      {ui::VirtualKey::kOem5, ImGuiKey_Backslash},
      {ui::VirtualKey::kOem1, ImGuiKey_Semicolon},
      {ui::VirtualKey::kOem7, ImGuiKey_Apostrophe},
      {ui::VirtualKey::kOemComma, ImGuiKey_Comma},
      {ui::VirtualKey::kOemPeriod, ImGuiKey_Period},
      {ui::VirtualKey::kOem2, ImGuiKey_Slash},
      // Numpad
      {ui::VirtualKey::kNumpad0, ImGuiKey_Keypad0},
      {ui::VirtualKey::kNumpad1, ImGuiKey_Keypad1},
      {ui::VirtualKey::kNumpad2, ImGuiKey_Keypad2},
      {ui::VirtualKey::kNumpad3, ImGuiKey_Keypad3},
      {ui::VirtualKey::kNumpad4, ImGuiKey_Keypad4},
      {ui::VirtualKey::kNumpad5, ImGuiKey_Keypad5},
      {ui::VirtualKey::kNumpad6, ImGuiKey_Keypad6},
      {ui::VirtualKey::kNumpad7, ImGuiKey_Keypad7},
      {ui::VirtualKey::kNumpad8, ImGuiKey_Keypad8},
      {ui::VirtualKey::kNumpad9, ImGuiKey_Keypad9},
      {ui::VirtualKey::kAdd, ImGuiKey_KeypadAdd},
      {ui::VirtualKey::kSubtract, ImGuiKey_KeypadSubtract},
      {ui::VirtualKey::kMultiply, ImGuiKey_KeypadMultiply},
      {ui::VirtualKey::kDivide, ImGuiKey_KeypadDivide},
      {ui::VirtualKey::kDecimal, ImGuiKey_KeypadDecimal},
  };
  if (auto search = map.find(vkey); search != map.end()) {
    return search->second;
  } else {
    return std::nullopt;
  }
}

void ImGuiDrawer::SetupFonts() {
  ImGuiIO& io = GetIO();
  io.Fonts->Clear();

  ImFontConfig font_config;
  font_config.OversampleH = font_config.OversampleV = 1;
  font_config.PixelSnapH = true;
  static const ImWchar font_glyph_ranges[] = {
      0x0020,
      0x00FF,  // Basic Latin + Latin Supplement
      0,
  };
  io.Fonts->AddFontFromMemoryCompressedBase85TTF(kProggyTinyCompressedDataBase85, 10.0f,
                                                 &font_config, font_glyph_ranges);

#if REX_PLATFORM_WIN32
  // TODO(benvanik): jp font on other platforms?
  // https://github.com/Koruri/kibitaki looks really good, but is 1.5MiB.
  const char* jp_font_path = "C:\\Windows\\Fonts\\msgothic.ttc";
  if (std::filesystem::exists(jp_font_path)) {
    ImFontConfig jp_font_config;
    jp_font_config.MergeMode = true;
    jp_font_config.OversampleH = jp_font_config.OversampleV = 1;
    jp_font_config.PixelSnapH = true;
    jp_font_config.FontNo = 0;
    io.Fonts->AddFontFromFileTTF(jp_font_path, 12.0f, &jp_font_config,
                                 io.Fonts->GetGlyphRangesJapanese());
  } else {
    REXLOG_WARN("Unable to load Japanese font; JP characters will be boxes");
  }
#endif

  if (font_setup_) {
    font_setup_(io.Fonts);
  }

  // UI fonts for styled overlays. Embedded Inter (OFL-licensed Latin subset,
  // fonts_inter.cpp) so every platform renders identically; system fonts are
  // only a fallback. With ImGuiBackendFlags_RendererHasTextures the size
  // given here is only the default - glyphs rasterize at whatever size is
  // pushed. MUST run after font_setup_: app callbacks (e.g. Skate3's
  // OnConfigureFonts) may Clear() the atlas, which deletes every ImFont
  // loaded before them - fonts cached here would dangle.
  {
    ImFontConfig config;
    // FreeType rasterization WITHOUT hinting: light hinting's blue-zone
    // snapping rounded the x-height down at the menu sizes and read as
    // vertically squished (playtest). Unhinted FreeType keeps the design
    // proportions exactly while still rasterizing curves cleaner than stb.
    // Advances stay fractional (browser letter spacing); the Oversample
    // fields are ignored by the FreeType loader.
    config.OversampleH = 1;
    config.OversampleV = 1;
    config.PixelSnapH = false;
    config.RasterizerMultiply = 1.0f;
    // Light-on-dark coverage gamma (rexglue imgui patch): browsers blend text
    // gamma-aware, which renders light glyphs on dark backgrounds fatter and
    // brighter than plain sRGB alpha blending: alpha^0.62 matches
    // DirectWrite white-on-black edge
    // profiles (identity was measurably thinner/dimmer). The *_on_light_
    // variants below keep gamma 1.0 - dark-on-light needs no correction.
    // NOTE: RasterizerGamma only exists in the author's patched imgui fork;
    // on stock imgui the correction is unavailable (fonts read slightly
    // thinner/dimmer on dark backgrounds).
    // config.RasterizerGamma = 0.62f;
    config.FontLoaderFlags = ImGuiFreeTypeBuilderFlags_NoHinting;
    ui_font_ = io.Fonts->AddFontFromMemoryCompressedBase85TTF(
        GetInterRegularCompressedBase85(), 18.0f, &config);
    ui_font_semibold_ = io.Fonts->AddFontFromMemoryCompressedBase85TTF(
        GetInterSemiBoldCompressedBase85(), 18.0f, &config);
    ui_font_bold_ = io.Fonts->AddFontFromMemoryCompressedBase85TTF(
        GetInterBoldCompressedBase85(), 18.0f, &config);
    // DARK-ON-LIGHT variants: naive sRGB alpha blending renders dark text on
    // light panels measurably fatter and softer than the browser (stems 4.94
    // vs 4.70 px, vertical edge gradient 73 vs 92) - the
    // browser's text engine gamma-adjusts coverage per polarity. A coverage
    // multiply < 1 approximates that for dark-on-light; light-on-dark keeps
    // the plain 1.0 fonts above. Metrics are identical across variants.
    ImFontConfig config_on_light = config;
    // 1.0 = currently identical to the plain fonts: the 0.8 coverage-thinning
    // experiment read as "lighter, not sharper" in playtest. Kept as the
    // per-polarity tuning knob.
    config_on_light.RasterizerMultiply = 1.0f;
    // Dark-on-light matches the browser with NO coverage curve (harness fit:
    // identity beat every contrast/gamma variant once bake==draw size).
    // config_on_light.RasterizerGamma = 1.0f;  // patched-imgui only, see above
    ui_font_on_light_ = io.Fonts->AddFontFromMemoryCompressedBase85TTF(
        GetInterRegularCompressedBase85(), 18.0f, &config_on_light);
    ui_font_semibold_on_light_ = io.Fonts->AddFontFromMemoryCompressedBase85TTF(
        GetInterSemiBoldCompressedBase85(), 18.0f, &config_on_light);
  }
  auto add_first_available = [&io](const std::vector<std::string>& paths) -> ImFont* {
    for (const std::string& path : paths) {
      if (!std::filesystem::exists(path)) {
        continue;
      }
      ImFontConfig config;
      config.FontNo = 0;
      if (ImFont* font = io.Fonts->AddFontFromFileTTF(path.c_str(), 18.0f, &config)) {
        return font;
      }
    }
    return nullptr;
  };
#if REX_PLATFORM_WIN32
  // Helvetica family first (per-user font dir too - user-installed fonts land
  // in %LOCALAPPDATA%, not C:\Windows\Fonts); Arial is a metric-compatible
  // Helvetica clone every Windows ships, Segoe UI is the last resort.
  std::string user_font_dir;
  if (const char* local_appdata = std::getenv("LOCALAPPDATA")) {
    user_font_dir = std::string(local_appdata) + "\\Microsoft\\Windows\\Fonts\\";
  }
  auto win_font_paths = [&user_font_dir](std::initializer_list<const char*> names) {
    std::vector<std::string> paths;
    for (const char* name : names) {
      if (!user_font_dir.empty()) {
        paths.push_back(user_font_dir + name);
      }
      paths.push_back(std::string("C:\\Windows\\Fonts\\") + name);
    }
    return paths;
  };
  if (!ui_font_) {
    ui_font_ = add_first_available(win_font_paths(
        {"HelveticaNowText-Regular.ttf", "HelveticaNowDisplay-Regular.ttf",
         "HelveticaNow-Regular.ttf", "Helvetica.ttf", "HelveticaNeue.ttf", "arial.ttf",
         "segoeui.ttf"}));
  }
  if (!ui_font_semibold_) {
    ui_font_semibold_ = add_first_available(win_font_paths(
        {"HelveticaNowText-Bold.ttf", "HelveticaNowDisplay-Bold.ttf", "HelveticaNow-Bold.ttf",
         "Helvetica-Bold.ttf", "HelveticaNeue-Bold.ttf", "arialbd.ttf", "seguisb.ttf",
         "segoeuib.ttf"}));
  }
#elif defined(__APPLE__)
  if (!ui_font_) {
    ui_font_ = add_first_available({"/System/Library/Fonts/Helvetica.ttc",
                                    "/System/Library/Fonts/Supplemental/Arial.ttf"});
  }
  if (!ui_font_semibold_) {
    ui_font_semibold_ =
        add_first_available({"/System/Library/Fonts/Supplemental/Arial Bold.ttf",
                             "/System/Library/Fonts/Helvetica.ttc"});
  }
#else
  if (!ui_font_) {
    ui_font_ = add_first_available(
        {"/usr/share/fonts/truetype/dejavu/DejaVuSans.ttf",
         "/usr/share/fonts/truetype/liberation/LiberationSans-Regular.ttf",
         "/usr/share/fonts/truetype/noto/NotoSans-Regular.ttf",
         "/usr/share/fonts/noto/NotoSans-Regular.ttf"});
  }
  if (!ui_font_semibold_) {
    ui_font_semibold_ = add_first_available(
        {"/usr/share/fonts/truetype/dejavu/DejaVuSans-Bold.ttf",
         "/usr/share/fonts/truetype/liberation/LiberationSans-Bold.ttf",
         "/usr/share/fonts/truetype/noto/NotoSans-Bold.ttf",
         "/usr/share/fonts/noto/NotoSans-Bold.ttf"});
  }
#endif
  if (!ui_font_) {
    REXLOG_WARN("No system UI font found; styled overlays use the default font");
  }
  if (!ui_font_semibold_) {
    ui_font_semibold_ = ui_font_;
  }
  if (!ui_font_bold_) {
    ui_font_bold_ = ui_font_semibold_;
  }
  if (!ui_font_on_light_) {
    ui_font_on_light_ = ui_font_;
  }
  if (!ui_font_semibold_on_light_) {
    ui_font_semibold_on_light_ = ui_font_semibold_;
  }
  REXLOG_INFO("imgui fonts: drawer={} atlas={} count={} ui={} uisb={}", (void*)this,
              (void*)io.Fonts, io.Fonts->Fonts.Size, (void*)ui_font_, (void*)ui_font_semibold_);
}

void ImGuiDrawer::SetupFontTexture() {
  if (font_texture_ || !immediate_drawer_) {
    return;
  }
  ImGuiIO& io = GetIO();
  if (io.BackendFlags & ImGuiBackendFlags_RendererHasTextures) {
    // Textures are created on demand in ProcessImGuiTextureRequests; the
    // legacy whole-atlas prebake must not run.
    return;
  }
  unsigned char* pixels;
  int width, height;
  io.Fonts->GetTexDataAsRGBA32(&pixels, &width, &height);
  font_texture_ = immediate_drawer_->CreateTexture(width, height, ImmediateTextureFilter::kLinear,
                                                   true, reinterpret_cast<uint8_t*>(pixels));
  io.Fonts->TexID = reinterpret_cast<ImTextureID>(font_texture_.get());
}

void ImGuiDrawer::SetPresenter(Presenter* new_presenter) {
  if (presenter_) {
    if (presenter_ == new_presenter) {
      return;
    }
    if (!dialogs_.empty()) {
      presenter_->RemoveUIDrawerFromUIThread(this);
    }
    ImGuiIO& io = GetIO();
  }
  presenter_ = new_presenter;
  if (presenter_) {
    if (!dialogs_.empty()) {
      presenter_->AddUIDrawerFromUIThread(this, z_order_);
    }
  }
}

void ImGuiDrawer::SetImmediateDrawer(ImmediateDrawer* new_immediate_drawer) {
  if (immediate_drawer_ == new_immediate_drawer) {
    return;
  }
  if (immediate_drawer_) {
    ImGuiIO& io = GetIO();
    io.Fonts->TexID = ImTextureID{};
    font_texture_.reset();
    if (io.BackendFlags & ImGuiBackendFlags_RendererHasTextures) {
      // Hand dynamic textures back to ImGui as destroyed so it re-requests
      // creation from the next immediate drawer.
      for (ImTextureData* tex : ImGui::GetPlatformIO().Textures) {
        if (tex->TexID != ImTextureID_Invalid) {
          tex->SetTexID(ImTextureID_Invalid);
          tex->SetStatus(ImTextureStatus_Destroyed);
        }
      }
      imgui_managed_textures_.clear();
    }
  }
  immediate_drawer_ = new_immediate_drawer;
  if (immediate_drawer_) {
    SetupFontTexture();
  }
}

void ImGuiDrawer::Draw(UIDrawContext& ui_draw_context) {
  // Drawing of anything is initiated by the presenter.
  assert_not_null(presenter_);
  if (!immediate_drawer_) {
    // A presenter has been attached, but an immediate drawer hasn't been
    // attached yet.
    return;
  }

  if (dialogs_.empty()) {
    return;
  }

  ImGui::SetCurrentContext(internal_state_);

  ImGuiIO& io = ImGui::GetIO();

  // The UI coordinate space is PHYSICAL pixels, 1:1 with the render target -
  // no logical->physical magnification anywhere in the UI path. Under a
  // fractional OS scale (e.g. Windows 150%) a logical coordinate space cannot
  // be blur-free: the GPU stretch lands half the pixel-snapped positions
  // between physical pixels, and glyph texels can never all align. Glyphs now
  // rasterize at their physical size directly, so density compensation
  // (DisplayFramebufferScale, the old Retina fix) must stay 1.
  io.DisplayFramebufferScale = ImVec2(1.0f, 1.0f);
  // Widget-based dialogs (message boxes, wizards, fps overlay) author their
  // font sizes in logical units - FontScaleDpi keeps their on-screen size
  // across DPI scales while text rasterizes at physical resolution. Explicit
  // ImDrawList::AddText sizes (the settings overlay) are unaffected by it.
  // Per-frame because the window can move between monitors with different
  // scales.
  ImGui::GetStyle().FontScaleDpi = float(window_->GetDpi()) / float(window_->GetMediumDpi());

  uint64_t current_frame_time_ticks = rex::chrono::Clock::QueryHostTickCount();
  io.DeltaTime =
      float(double(current_frame_time_ticks - last_frame_time_ticks_) / frame_time_tick_frequency_);
  if (!(io.DeltaTime > 0.0f) || current_frame_time_ticks < last_frame_time_ticks_) {
    // For safety as Dear ImGui doesn't allow non-positive DeltaTime. Using the
    // same default value as in the official samples.
    io.DeltaTime = 1.0f / 60.0f;
  }
  last_frame_time_ticks_ = current_frame_time_ticks;

  io.DisplaySize.x = float(window_->GetActualPhysicalWidth());
  io.DisplaySize.y = float(window_->GetActualPhysicalHeight());

  ImGui::NewFrame();

  assert_true(!IsDrawingDialogs());
  dialog_loop_next_index_ = 0;
  while (dialog_loop_next_index_ < dialogs_.size()) {
    dialogs_[dialog_loop_next_index_++]->Draw();
  }
  dialog_loop_next_index_ = SIZE_MAX;

  ImGui::Render();
  ImDrawData* draw_data = ImGui::GetDrawData();
  if (draw_data) {
    RenderDrawLists(draw_data, ui_draw_context);
  }

  if (reset_mouse_position_after_next_frame_) {
    reset_mouse_position_after_next_frame_ = false;
    io.AddMousePosEvent(-FLT_MAX, -FLT_MAX);
  }

  // Keep the platform text input state in sync with whether a text widget is
  // active (SDL only delivers character events while text input is started).
  bool want_text_input = io.WantTextInput && !dialogs_.empty();
  if (want_text_input != text_input_active_) {
    text_input_active_ = want_text_input;
    window_->SetTextInputActive(want_text_input);
  }

  // Detaching is deferred if the last dialog is removed during drawing, perform
  // it now if needed.
  DetachIfLastDialogRemoved();

  if (std::any_of(dialogs_.cbegin(), dialogs_.cend(),
                  [](const ImGuiDialog* dialog) { return dialog->WantsContinuousRepaint(); })) {
    // Repaint (and handle input) continuously if still active.
    presenter_->RequestUIPaintFromUIThread();
  }
}

void ImGuiDrawer::ProcessImGuiTextureRequests(ImDrawData* data) {
  if (!data->Textures) {
    return;
  }
  for (ImTextureData* tex : *data->Textures) {
    switch (tex->Status) {
      case ImTextureStatus_WantCreate:
      case ImTextureStatus_WantUpdates: {
        // The ImmediateDrawer has no partial-update API - recreate the whole
        // texture from ImGui's CPU-side copy. This only happens when new
        // glyph sizes/densities are first drawn, then the atlas settles.
        assert_true(tex->Format == ImTextureFormat_RGBA32);
        auto texture = immediate_drawer_->CreateTexture(
            uint32_t(tex->Width), uint32_t(tex->Height), ImmediateTextureFilter::kLinear, true,
            static_cast<const uint8_t*>(tex->GetPixels()));
        if (tex->TexID != ImTextureID_Invalid) {
          auto* old_texture = reinterpret_cast<ImmediateTexture*>(tex->TexID);
          std::erase_if(imgui_managed_textures_,
                        [old_texture](const auto& t) { return t.get() == old_texture; });
        }
        tex->SetTexID(reinterpret_cast<ImTextureID>(texture.get()));
        tex->SetStatus(ImTextureStatus_OK);
        imgui_managed_textures_.push_back(std::move(texture));
        break;
      }
      case ImTextureStatus_WantDestroy: {
        if (tex->UnusedFrames < 1) {
          break;
        }
        auto* old_texture = reinterpret_cast<ImmediateTexture*>(tex->TexID);
        std::erase_if(imgui_managed_textures_,
                      [old_texture](const auto& t) { return t.get() == old_texture; });
        tex->SetTexID(ImTextureID_Invalid);
        tex->SetStatus(ImTextureStatus_Destroyed);
        break;
      }
      default:
        break;
    }
  }
}

void ImGuiDrawer::RenderDrawLists(ImDrawData* data, UIDrawContext& ui_draw_context) {
  ImGuiIO& io = ImGui::GetIO();

  if (io.BackendFlags & ImGuiBackendFlags_RendererHasTextures) {
    ProcessImGuiTextureRequests(data);
  }

  immediate_drawer_->Begin(ui_draw_context, io.DisplaySize.x, io.DisplaySize.y);

  for (int i = 0; i < data->CmdListsCount; ++i) {
    const auto cmd_list = data->CmdLists[i];

    ImmediateDrawBatch batch;
    batch.vertices = reinterpret_cast<ImmediateVertex*>(cmd_list->VtxBuffer.Data);
    batch.vertex_count = cmd_list->VtxBuffer.size();
    batch.indices = cmd_list->IdxBuffer.Data;
    batch.index_count = cmd_list->IdxBuffer.size();
    immediate_drawer_->BeginDrawBatch(batch);

    for (int j = 0; j < cmd_list->CmdBuffer.size(); ++j) {
      const auto& cmd = cmd_list->CmdBuffer[j];

      ImmediateDraw draw;
      draw.primitive_type = ImmediatePrimitiveType::kTriangles;
      draw.count = cmd.ElemCount;
      draw.index_offset = cmd.IdxOffset;
      draw.texture = reinterpret_cast<ImmediateTexture*>(cmd.GetTexID());
      draw.scissor = true;
      draw.scissor_left = cmd.ClipRect.x;
      draw.scissor_top = cmd.ClipRect.y;
      draw.scissor_right = cmd.ClipRect.z;
      draw.scissor_bottom = cmd.ClipRect.w;
      immediate_drawer_->Draw(draw);
    }

    immediate_drawer_->EndDrawBatch();
  }

  immediate_drawer_->End();
}

ImGuiIO& ImGuiDrawer::GetIO() {
  ImGui::SetCurrentContext(internal_state_);
  return ImGui::GetIO();
}

void ImGuiDrawer::OnKeyDown(KeyEvent& e) {
  OnKey(e, true);
}

void ImGuiDrawer::OnKeyUp(KeyEvent& e) {
  OnKey(e, false);
}

void ImGuiDrawer::OnKeyChar(KeyEvent& e) {
  auto& io = GetIO();
  // TODO(Triang3l): Accept the Unicode character.
  unsigned int character = static_cast<unsigned int>(e.virtual_key());
  if (character > 0 && character < 0x10000) {
    io.AddInputCharacter(character);
    e.set_handled(true);
  }
}

int ImGuiDrawer::MouseEventButtonToImGui(const MouseEvent& e) {
  switch (e.button()) {
    case rex::ui::MouseEvent::Button::kLeft:
      return 0;
    case rex::ui::MouseEvent::Button::kRight:
      return 1;
    default:
      // Ignored.
      return -1;
  }
}

void ImGuiDrawer::OnMouseDown(MouseEvent& e) {
  SwitchToPhysicalMouseAndUpdateMousePosition(e);
  auto& io = GetIO();
  int button = MouseEventButtonToImGui(e);
  if (button >= 0) {
    if (!(mouse_buttons_down_ & (UINT32_C(1) << button))) {
      if (!mouse_buttons_down_) {
        window_->CaptureMouse();
      }
      mouse_buttons_down_ |= UINT32_C(1) << button;
    }
    // Queue rather than write io.MouseDown directly: the event queue keeps the
    // position-then-button ordering and spreads a same-frame press+release
    // over multiple NewFrames, so clicks register at the position they
    // actually happened at and fast clicks aren't lost when the UI frame rate
    // lags behind input.
    io.AddMouseButtonEvent(button, true);
    if (io.WantCaptureMouse) {
      // Keep presses over the UI out of lower-Z listeners (game input,
      // keybinds). Releases are deliberately never eaten so lower listeners
      // can't be left with a stuck button.
      e.set_handled(true);
    }
  }
}

void ImGuiDrawer::OnMouseMove(MouseEvent& e) {
  SwitchToPhysicalMouseAndUpdateMousePosition(e);
}

void ImGuiDrawer::OnMouseUp(MouseEvent& e) {
  SwitchToPhysicalMouseAndUpdateMousePosition(e);
  auto& io = GetIO();
  int button = MouseEventButtonToImGui(e);
  if (button >= 0) {
    if (mouse_buttons_down_ & (UINT32_C(1) << button)) {
      mouse_buttons_down_ &= ~(UINT32_C(1) << button);
      if (!mouse_buttons_down_) {
        window_->ReleaseMouse();
      }
    }
    io.AddMouseButtonEvent(button, false);
  }
}

void ImGuiDrawer::OnMouseWheel(MouseEvent& e) {
  SwitchToPhysicalMouseAndUpdateMousePosition(e);
  auto& io = GetIO();
  io.AddMouseWheelEvent(0.0f, float(e.scroll_y()) / float(MouseEvent::kScrollPerDetent));
  if (io.WantCaptureMouse) {
    e.set_handled(true);
  }
}

void ImGuiDrawer::OnTouchEvent(TouchEvent& e) {
  auto& io = GetIO();
  TouchEvent::Action action = e.action();
  uint32_t pointer_id = e.pointer_id();
  if (action == TouchEvent::Action::kDown) {
    // The latest pointer needs to be controlling the ImGui mouse.
    if (touch_pointer_id_ == TouchEvent::kPointerIDNone) {
      // Switching from the mouse to touch input.
      if (mouse_buttons_down_) {
        for (int button = 0; button < 32; ++button) {
          if (mouse_buttons_down_ & (UINT32_C(1) << button)) {
            io.AddMouseButtonEvent(button, false);
          }
        }
        mouse_buttons_down_ = 0;
        window_->ReleaseMouse();
      }
    }
    touch_pointer_id_ = pointer_id;
  } else {
    if (pointer_id != touch_pointer_id_) {
      return;
    }
  }
  UpdateMousePosition(e.x(), e.y());
  if (action == TouchEvent::Action::kUp || action == TouchEvent::Action::kCancel) {
    io.AddMouseButtonEvent(0, false);
    touch_pointer_id_ = TouchEvent::kPointerIDNone;
    // Make sure that after a touch, the ImGui mouse isn't hovering over
    // anything.
    reset_mouse_position_after_next_frame_ = true;
  } else {
    io.AddMouseButtonEvent(0, true);
    reset_mouse_position_after_next_frame_ = false;
  }
}

void ImGuiDrawer::ClearInput() {
  auto& io = GetIO();
  if (touch_pointer_id_ == TouchEvent::kPointerIDNone && mouse_buttons_down_) {
    window_->ReleaseMouse();
  }
  mouse_buttons_down_ = 0;
  io.ClearEventsQueue();
  io.ClearInputKeys();
  io.ClearInputMouse();
  touch_pointer_id_ = TouchEvent::kPointerIDNone;
  reset_mouse_position_after_next_frame_ = false;
  if (text_input_active_) {
    text_input_active_ = false;
    window_->SetTextInputActive(false);
  }
}

void ImGuiDrawer::OnKey(KeyEvent& e, bool is_down) {
  auto& io = GetIO();
  const VirtualKey virtual_key = e.virtual_key();
  if (auto imGuiKey = VirtualKeyToImGuiKey(virtual_key); imGuiKey) {
    io.AddKeyEvent(*imGuiKey, is_down);
  }
  switch (virtual_key) {
    case VirtualKey::kShift:
    case VirtualKey::kLShift:
    case VirtualKey::kRShift:
      io.AddKeyEvent(ImGuiMod_Shift, is_down);
      break;
    case VirtualKey::kControl:
    case VirtualKey::kLControl:
    case VirtualKey::kRControl:
      io.AddKeyEvent(ImGuiMod_Ctrl, is_down);
      break;
    case VirtualKey::kMenu:
    case VirtualKey::kLMenu:
    case VirtualKey::kRMenu:
      io.AddKeyEvent(ImGuiMod_Alt, is_down);
      break;
    case VirtualKey::kLWin:
    case VirtualKey::kRWin:
      io.AddKeyEvent(ImGuiMod_Super, is_down);
      break;
    default:
      break;
  }
  // While a text field is active, keep key presses away from app keybinds and
  // the game (typing a name must not toggle overlays or drive MnK input).
  // Releases always propagate so lower listeners can't get stuck keys.
  if (is_down && io.WantTextInput) {
    e.set_handled(true);
  }
}

void ImGuiDrawer::UpdateMousePosition(float x, float y) {
  auto& io = GetIO();
  // MouseEvents already carry physical pixels (WindowPointToPhysical) - the
  // UI coordinate space is physical too, no conversion.
  io.AddMousePosEvent(x, y);
}

void ImGuiDrawer::SwitchToPhysicalMouseAndUpdateMousePosition(const MouseEvent& e) {
  if (touch_pointer_id_ != TouchEvent::kPointerIDNone) {
    touch_pointer_id_ = TouchEvent::kPointerIDNone;
    auto& io = GetIO();
    // Release the ImGui touch-driven button.
    io.AddMouseButtonEvent(0, false);
    // Nothing needs to be done regarding CaptureMouse and ReleaseMouse - all
    // buttons as well as mouse capture have been released when switching to
    // touch input, the mouse is never captured during touch input, and now
    // resetting to no buttons down (therefore not capturing).
  }
  reset_mouse_position_after_next_frame_ = false;
  UpdateMousePosition(float(e.x()), float(e.y()));
}

void ImGuiDrawer::DetachIfLastDialogRemoved() {
  // IsDrawingDialogs() is also checked because in a situation of removing the
  // only dialog, then adding a dialog, from within a dialog's Draw function,
  // re-registering the ImGuiDrawer may result in ImGui being drawn multiple
  // times in the current frame.
  if (!dialogs_.empty() || IsDrawingDialogs()) {
    return;
  }
  if (presenter_) {
    presenter_->RemoveUIDrawerFromUIThread(this);
  }
  window_->RemoveInputListener(this);
  // Clear all input since no input will be received anymore, and when the
  // drawer becomes active again, it'd have an outdated input state otherwise
  // which will be persistent until new events actualize individual input
  // properties.
  ClearInput();
}

}  // namespace ui
}  // namespace rex
