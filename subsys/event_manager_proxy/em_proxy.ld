event_manager_type_size_section 0 (DSECT) :
{
	KEEP(*(event_manager_type_size));
	event_manager_type_size = .;
}

event_manager_proxy_type_size_section 0 (DSECT) :
{
	KEEP(*(event_manger_proxy_type_ptr_size));
	event_manager_proxy_type_ptr_size = .;
}

event_proxy_array : ALIGN_WITH_INPUT
{
	__start_event_proxy_array = .;
	event_manager_proxy_array = .;
	. = . + (_event_type_list_end - _event_type_list_start)
		* event_manager_proxy_type_ptr_size / event_manager_type_size;
	__end_event_proxy_array = .;
} GROUP_LINK_IN(RAMABLE_REGION)
