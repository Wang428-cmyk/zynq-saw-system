# Vivado 2021.2 batch entry: restore sources first using tools/prepare_sources.py
set root [file normalize [file join [file dirname [info script]] .. ..]]
if {[version -short] ne "2021.2"} {error "Use Vivado 2021.2 for this build profile."}
set proj [file join $root src pl native Single_Channel_Simulator Single_Channel_Simulator.xpr]
if {![file exists $proj]} {
    error "Run 'python tools/prepare_sources.py' from the package root before running Vivado."
}
open_project $proj
set expected_part "xc7z035ffg676-2"
if {[get_property PART [current_project]] ne $expected_part} {
    error "Project device differs from the recorded part $expected_part"
}
update_compile_order -fileset sources_1
set bds [get_files -quiet [file join $root src pl native Single_Channel_Simulator Single_Channel_Simulator.srcs sources_1 bd System System.bd]]
if {[llength $bds] > 0} {
    foreach bd $bds {
        # The archive retains the historical 'validated' flag but omits generated
        # products. Re-propagate connected interface parameters before building
        # SmartConnect; trusting that flag can produce an empty nested design.
        open_bd_design $bd
        validate_bd_design -force
        save_bd_design
        # Recreate the complete BD hierarchy from source, rather than relying on
        # historical out-of-context SmartConnect checkpoints absent from ZIP.
        set_property synth_checkpoint_mode None $bd
        reset_target all $bd
        generate_target all $bd
    }
}
set output [file join $root build generated]
file mkdir $output
# Native XPR retains a 2020 implementation flow. Recreate both top-level runs
# using the installed 2021 flow; otherwise an empty implementation script can
# be generated and wait_on_run can wait indefinitely. RTL/XDC/IP are unchanged.
create_run saw_synth -part $expected_part -srcset sources_1 -constrset constrs_1 -flow {Vivado Synthesis 2021} -strategy {Vivado Synthesis Defaults}
create_run saw_impl -part $expected_part -parent_run saw_synth -constrset constrs_1 -flow {Vivado Implementation 2021} -strategy {Vivado Implementation Defaults}
launch_runs saw_synth -jobs 4
wait_on_run saw_synth -timeout 60
if {![string match "*Complete*" [get_property STATUS [get_runs saw_synth]]]} {error "Synthesis did not complete: [get_property STATUS [get_runs saw_synth]]"}
open_run saw_synth
report_utilization -file [file join $output utilization_synth.rpt]
close_design
launch_runs saw_impl -to_step write_bitstream -jobs 4
wait_on_run saw_impl -timeout 60
if {![string match "*Complete*" [get_property STATUS [get_runs saw_impl]]]} {error "Implementation did not complete: [get_property STATUS [get_runs saw_impl]]"}
open_run saw_impl
report_utilization -file [file join $output utilization_impl.rpt]
report_timing_summary -file [file join $output timing_summary_impl.rpt]
report_drc -file [file join $output drc_impl.rpt]
write_hw_platform -fixed -include_bit -force -file [file join $output top_module.xsa]
close_design
close_project
puts "SAW_PL_BUILD_SUCCESS"
