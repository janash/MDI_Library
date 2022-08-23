/*! \file
 *
 * \brief Implementation of library-based communication
 */
#include <errno.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <signal.h>
#include "mdi.h"
#include "mdi_lib.h"
#include "mdi_global.h"
#include "mdi_general.h"
#include "mdi_plug_py.h"

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

/*! \brief Shared state received from the driver */
plugin_shared_state* shared_state_from_driver = NULL;

/*! \brief Enable support for the PLUG method */
int enable_plug_support( int code_id ) {
  new_method(code_id, MDI_LINK);
  method* this_method = get_method(code_id, MDI_LINK);
  this_method->on_selection = plug_on_selection;
  this_method->on_accept_communicator = plug_on_accept_communicator;
  this_method->on_send_command = plug_on_send_command;
  this_method->after_send_command = plug_after_send_command;
  this_method->on_recv_command = plug_on_recv_command;
  return 0;
}



/*! \brief Callback when the end-user selects PLUG as the method */
int plug_on_selection() {
  code* this_code = get_code(current_code);

  // Check if this is an engine being used as a library
  if (strcmp(this_code->role, "ENGINE") == 0) {
    this_code->is_library = 1;
    library_initialize();
  }

  return 0;
}



/*! \brief Callback when the PLUG method must accept a communicator */
int plug_on_accept_communicator() {
  code* this_code = get_code(current_code);

  // Give the library method an opportunity to update the current code
  library_accept_communicator();

  // If MDI hasn't returned some connections, do that now
  if ( this_code->returned_comms < this_code->next_comm - 1 ) {
    this_code->returned_comms++;
    communicator* comm_obj = get_communicator(current_code, this_code->returned_comms);
    comm_obj->is_accepted = 1;
    return this_code->returned_comms;
  }

  // unable to accept any connections
  return MDI_COMM_NULL;
}



/*! \brief Callback when the PLUG method must send a command */
int plug_on_send_command(const char* command, MDI_Comm comm, int* skip_flag) {
  code* this_code = get_code(current_code);
  communicator* this_comm = get_communicator(current_code, comm);
  library_data* libd = (library_data*) this_comm->method_data;
  //int iengine = libd->connected_code;
  //code* engine_code = get_code(iengine);
  int ret = 0;

  // Check whether MPI has been initialized
  int mpi_init_flag;
  if ( MPI_Initialized(&mpi_init_flag) ) {
    mdi_error("Error in MDI_plug_on_send_command: MPI_Initialized failed");
    return 1;
  }

  // broadcast the command to each rank
  char* command_bcast = malloc( MDI_COMMAND_LENGTH * sizeof(char) );
  //if ( engine_code->intra_rank == 0 ) {
    int ichar;
    for ( ichar=0; ichar < MDI_COMMAND_LENGTH; ichar++) {
      command_bcast[ichar] = '\0';
    }
    for ( ichar=0; ichar < strlen(command) && ichar < MDI_COMMAND_LENGTH; ichar++ ) {
      command_bcast[ichar] = command[ichar];
    }
  //}
  /*
  if ( mpi_init_flag == 1) {
    //MPI_Bcast( &command_bcast[0], MDI_COMMAND_LENGTH, MPI_CHAR, 0, engine_code->intra_MPI_comm);
    MPI_Bcast( &command_bcast[0], MDI_COMMAND_LENGTH, MPI_CHAR, 0, *(MPI_Comm*)libd->shared_state->mpi_comm_ptr);
  }
  */

  // ensure that the driver is the current code
  library_set_driver_current(comm);

  // set the command for the engine to execute
  library_set_command(command_bcast, comm);

  if ( command_bcast[0] == '<' ) {
    // execute the command, so that the data from the engine can be received later by the driver
    //ret = library_execute_command(comm);
    libd->shared_state->lib_execute_command(libd->shared_state->engine_mdi_comm);
    if ( ret != 0 ) {
      mdi_error("Error in MDI_Send_Command: Unable to execute receive command through library");
      free( command_bcast );
      return ret;
    }
    *skip_flag = 1;
  }
  else if ( command_bcast[0] == '>' ) {
    // flag the command to be executed after the next call to MDI_Send
    communicator* this = get_communicator(current_code, comm);
    library_data* libd = (library_data*) this->method_data;
    libd->execute_on_send = 1;
    *skip_flag = 1;
  }
  else if ( plugin_mode && ( strcmp( command_bcast, "EXIT" ) == 0 || command_bcast[0] == '@' ) ) {
    // this command should be received by MDI_Recv_command, rather than through the execute_command callback
  }
  else {
    // this is a command that neither sends nor receives data, so execute it now
    //ret = library_execute_command(comm);
    libd->shared_state->lib_execute_command(libd->shared_state->engine_mdi_comm);
    if ( ret != 0 ) {
      mdi_error("Error in MDI_Send_Command: Unable to execute command through library");
      free( command_bcast );
      return ret;
    }
    *skip_flag = 1;
  }

  free( command_bcast );
  return ret;
}



/*! \brief Callback after the PLUG method has received a command */
int plug_after_send_command(const char* command, MDI_Comm comm) {
  return 0;
}



/*! \brief Callback when the PLUG method must receive a command */
int plug_on_recv_command(MDI_Comm comm) {
  int ret = 0;
  int iengine = current_code;
  code* this_code = get_code(current_code);
  communicator* engine_comm = get_communicator(current_code, comm);

  // get the driver code to which this communicator connects
  library_data* libd = (library_data*) engine_comm->method_data;
  int idriver = libd->connected_code;

  //MDI_Comm driver_comm_handle = library_get_matching_handle(comm);
  //communicator* driver_comm = get_communicator(idriver, driver_comm_handle);
  //library_data* driver_lib = (library_data*) driver_comm->method_data;

  // Copy the execute_command function to a shared location
  libd->shared_state->execute_command = this_code->execute_command;
  libd->shared_state->execute_command_obj = this_code->execute_command_obj;

  // set the current code to the driver
//  current_code = idriver;
  current_code = libd->shared_state->driver_code_id;

  //void* class_obj = driver_lib->driver_callback_obj;
  ret = libd->shared_state->driver_node_callback(libd->shared_state->mpi_comm_ptr, libd->shared_state->driver_mdi_comm, libd->shared_state->driver_callback_obj);
  if ( ret != 0 ) {
    mdi_error("PLUG error in on_recv_command: driver_node_callback failed");
    return ret;
  }

  // set the current code to the engine
//  current_code = iengine;
  current_code = libd->shared_state->engine_code_id;

  return 0;
}



/*! \brief Load the initialization function for a plugin
 *
 */
int library_load_init(const char* plugin_name, void* mpi_comm_ptr,
                      library_data* libd, int mode) {
  int ret;
  int driver_code_id = current_code;
  code* this_code = get_code(driver_code_id);
  MPI_Comm mpi_comm = *(MPI_Comm*) mpi_comm_ptr;

  //
  // Get the path to the plugin
  // Note: Eventually, should probably replace this code with libltdl
  //
  char* plugin_path = malloc( PLUGIN_PATH_LENGTH * sizeof(char) );

  // Get the name of the plugin's init function
  char* plugin_init_name = malloc( PLUGIN_PATH_LENGTH * sizeof(char) );
  if ( mode == 0 ) { // Load MDI_Plugin_init
    snprintf(plugin_init_name, PLUGIN_PATH_LENGTH, "MDI_Plugin_init_%s", plugin_name);
  }
  else { // Load MDI_Plugin_open
    snprintf(plugin_init_name, PLUGIN_PATH_LENGTH, "MDI_Plugin_open_%s", plugin_name);
  }

  // Attempt to load a python script
  snprintf(plugin_path, PLUGIN_PATH_LENGTH, "%s/%s.py", this_code->plugin_path, plugin_name);
  if ( file_exists(plugin_path) ) {
    libd->is_python = 1;
    libd->shared_state->engine_language = MDI_LANGUAGE_PYTHON;
    ret = python_plugin_init( plugin_name, plugin_path, mpi_comm_ptr, libd->shared_state );
    if ( ret != 0 ) {
      mdi_error("Error in python_plugin_init");
      return -1;
    }
  }
  else {
    libd->is_python = 0;

#ifdef _WIN32
  // Attempt to open a library with a .dll extension
  snprintf(plugin_path, PLUGIN_PATH_LENGTH, "%s/lib%s.dll", this_code->plugin_path, plugin_name);
  libd->plugin_handle = LoadLibrary( plugin_path );
  if ( ! libd->plugin_handle ) {
    // Unable to find the plugin library
    free( plugin_path );
    mdi_error("Unable to open MDI plugin");
    return -1;
  }

  // Load a plugin's initialization function
  libd->plugin_init = (MDI_Plugin_init_t) (intptr_t) GetProcAddress( libd->plugin_handle, plugin_init_name );
  if ( ! libd->plugin_init ) {
    free( plugin_path );
    free( plugin_init_name );
    FreeLibrary( libd->plugin_handle );
    mdi_error("Unable to load MDI plugin init function");
    return -1;
  }

#else
  // Attempt to open a library with a .so extension
  snprintf(plugin_path, PLUGIN_PATH_LENGTH, "%s/lib%s.so", this_code->plugin_path, plugin_name);
  libd->plugin_handle = dlopen(plugin_path, RTLD_NOW);
  if ( ! libd->plugin_handle ) {

    // Attempt to open a library with a .dylib extension
    snprintf(plugin_path, PLUGIN_PATH_LENGTH, "%s/lib%s.dylib", this_code->plugin_path, plugin_name);
    libd->plugin_handle = dlopen(plugin_path, RTLD_NOW);
    if ( ! libd->plugin_handle ) {
      // Unable to find the plugin library
      free( plugin_path );
      free( plugin_init_name );
      mdi_error("Unable to open MDI plugin");
      return -1;
    }
  }

  // Load a plugin's initialization function
  libd->plugin_init = (MDI_Plugin_init_t) (intptr_t) dlsym(libd->plugin_handle, plugin_init_name);
  if ( ! libd->plugin_init ) {
    free( plugin_path );
    free( plugin_init_name );
    dlclose( libd->plugin_handle );
    mdi_error("Unable to load MDI plugin init function");
    return -1;
  }
#endif

    // Initialize an instance of the plugin
    ret = libd->plugin_init( libd->shared_state );
    if ( ret != 0 ) {
      mdi_error("MDI plugin init function returned non-zero exit code");
      return -1;
    }

  }

  // free memory from loading the plugin's initialization function
  free( plugin_path );
  free( plugin_init_name );

  return 0;
}



/*! \brief Parse command-line plugin options
 *
 */
int library_parse_options(const char* options, library_data* libd) {

  // Begin parsing the options char array into an argv-style array of char arrays

  // copy the input options array
  int options_len = strlen(options) + 1;
  libd->plugin_options = malloc( options_len * sizeof(char) );
  snprintf(libd->plugin_options, options_len, "%s", options);
  libd->plugin_unedited_options = malloc( options_len * sizeof(char) );
  snprintf(libd->plugin_unedited_options, options_len, "%s", options);
  libd->plugin_options_allocated = 1;

  // determine the number of arguments
  libd->shared_state->plugin_argc = 0;
  int ichar;
  int in_argument = 0; // was the previous character part of an argument, or just whitespace?
  int in_single_quotes = 0; // was the previous character part of a single quote?
  int in_double_quotes = 0; // was the previous character part of a double quote?
  for (ichar=0; ichar < options_len; ichar++) {
    if ( libd->plugin_options[ichar] == '\0' ) {
      if ( in_double_quotes ) {
        mdi_error("Unterminated double quotes received in MDI_Launch_plugin \"options\" argument.");
      }
      if ( in_argument ) {
        libd->shared_state->plugin_argc++;
      }
      in_argument = 0;
    }
    else if (libd->plugin_options[ichar] == ' ') {
      if ( ! in_double_quotes && ! in_single_quotes ) {
        if ( in_argument ) {
          libd->shared_state->plugin_argc++;
        }
        in_argument = 0;
        libd->plugin_options[ichar] = '\0';
      }
    }
    else if (libd->plugin_options[ichar] == '\"') {
      if ( in_single_quotes ) {
        mdi_error("Nested quotes not supported by MDI_Launch_plugin \"options\" argument.");
      }
      in_argument = 1;
      in_double_quotes = (in_double_quotes + 1) % 2;
      libd->plugin_options[ichar] = '\0';
    }
    else if (libd->plugin_options[ichar] == '\'') { 
      if ( in_double_quotes ) {
        mdi_error("Nested quotes not supported by MDI_Launch_plugin \"options\" argument.");
      }
      in_argument = 1;
      in_single_quotes = (in_single_quotes + 1) % 2;
      libd->plugin_options[ichar] = '\0';
    }
    else {
      in_argument = 1;
    }
  }

  // construct pointers to all of the arguments
  libd->shared_state->plugin_argv = malloc( libd->shared_state->plugin_argc * sizeof(char*) );
  libd->shared_state->plugin_argv_allocated = 1;
  int iarg = 0;
  for (ichar=0; ichar < options_len; ichar++) {
    if ( libd->plugin_options[ichar] != '\0' ) {
      if ( ichar == 0 || libd->plugin_options[ichar-1] == '\0' ) {
        libd->shared_state->plugin_argv[iarg] = &libd->plugin_options[ichar];
        iarg++;
      }
    }
  }
  if ( iarg != libd->shared_state->plugin_argc ) {
    mdi_error("Programming error: unable to correctly parse the MDI_Launch_plugin \"options\" argument.");
  }

  return 0;
}



/*! \brief Launch an MDI plugin
 *
 */
int library_launch_plugin(const char* plugin_name, const char* options, void* mpi_comm_ptr,
                          MDI_Driver_node_callback_t driver_node_callback,
                          void* driver_callback_object) {
  int ret;
  code* this_code = get_code(current_code);
  MPI_Comm mpi_comm = *(MPI_Comm*) mpi_comm_ptr;




  // initialize a communicator for the driver
  int icomm = library_initialize();
  communicator* driver_comm = get_communicator(current_code, icomm);
  library_data* libd = (library_data*) driver_comm->method_data;
  libd->connected_code = (int)codes.size;

  // allocate data that is shared between the driver and the plugin
  libd->shared_state = malloc(sizeof(plugin_shared_state));
  libd->shared_state->plugin_argc = 0;
  libd->shared_state->plugin_argv_allocated = 0;
  libd->shared_state->buf_allocated = 0;
  libd->shared_state->driver_code_id = current_code;
  libd->shared_state->engine_language = MDI_LANGUAGE_C;

  MDI_Comm comm;
  ret = MDI_Accept_Communicator(&comm);
  if ( ret != 0 || comm == MDI_COMM_NULL ) {
    mdi_error("MDI unable to create communicator for plugin");
    return -1;
  }

  // Set the driver callback function to be used by this plugin instance
  libd->driver_callback_obj = driver_callback_object;
  libd->driver_node_callback = driver_node_callback;

  // Set the mpi communicator associated with this plugin instance
  libd->mpi_comm = mpi_comm;

  // Parse plugin command-line options
  library_parse_options(options, libd);

  // Assign the global command-line options variables to the values for this plugin  
  plugin_argc = libd->shared_state->plugin_argc;
  plugin_argv = libd->shared_state->plugin_argv;


  libd->shared_state->mpi_comm_ptr = &libd->mpi_comm;
  libd->shared_state->driver_node_callback = libd->driver_node_callback;
  libd->shared_state->driver_mdi_comm = driver_comm->id;
  libd->shared_state->driver_callback_obj = libd->driver_callback_obj;

  //
  // Get the path to the plugin
  // Note: Eventually, should probably replace this code with libltdl
  //
  char* plugin_path = malloc( PLUGIN_PATH_LENGTH * sizeof(char) );

  // Get the name of the plugin's init function
  char* plugin_init_name = malloc( PLUGIN_PATH_LENGTH * sizeof(char) );
  snprintf(plugin_init_name, PLUGIN_PATH_LENGTH, "MDI_Plugin_init_%s", plugin_name);




  /*************************************************/
  /*************** BEGIN PLUGIN MODE ***************/
  /*************************************************/
  plugin_mode = 1;

  ret = library_load_init(plugin_name, mpi_comm_ptr, libd, 0);
  if ( ret != 0 ) {
    free( plugin_path );
    free( plugin_init_name );
    return ret;
  }

  // store a couple of values from libd, before we delete it
  int is_python = libd->is_python;
#ifdef _WIN32
  HINSTANCE plugin_handle = libd->plugin_handle;
#else
  void* plugin_handle = libd->plugin_handle;
#endif

  plugin_mode = 0;
//  current_code = libd->shared_state->driver_code_id;
  current_code = libd->shared_state->driver_code_id;

  // Delete the driver's communicator to the engine
  // This will also delete the engine code and its communicator
  delete_communicator(libd->shared_state->driver_code_id, comm);

  if (is_python == 0 ) {
  // Close the plugin library
#ifdef _WIN32
    FreeLibrary( plugin_handle );
#else
    dlclose( plugin_handle );
#endif
  }

  /*************************************************/
  /**************** END PLUGIN MODE ****************/
  /*************************************************/


  // free memory from loading the plugin's initialization function
  free( plugin_path );
  free( plugin_init_name );

  return 0;
}



/*! \brief Open an MDI plugin in the background
 *
 */
int library_open_plugin(const char* plugin_name, const char* options, void* mpi_comm_ptr,
                          MDI_Comm* mdi_comm_ptr) {
  int ret;
  int driver_code_id = current_code;
  code* this_code = get_code(driver_code_id);
  MPI_Comm mpi_comm = *(MPI_Comm*) mpi_comm_ptr;

  // initialize a communicator for the driver
  int icomm = library_initialize();
  communicator* driver_comm = get_communicator(current_code, icomm);
  library_data* libd = (library_data*) driver_comm->method_data;
  libd->connected_code = (int)codes.size;

  MDI_Comm comm;
  ret = MDI_Accept_Communicator(&comm);
  if ( ret != 0 || comm == MDI_COMM_NULL ) {
    mdi_error("MDI unable to create communicator for plugin");
    return -1;
  }

  // Set the mpi communicator associated with this plugin instance
  libd->mpi_comm = mpi_comm;

  // Parse plugin command-line options
  library_parse_options(options, libd);

  // Assign the global command-line options variables to the values for this plugin  
  plugin_argc = libd->shared_state->plugin_argc;
  plugin_argv = libd->shared_state->plugin_argv;
  plugin_unedited_options = libd->plugin_unedited_options;

  //
  // Get the path to the plugin
  // Note: Eventually, should probably replace this code with libltdl
  //
  char* plugin_path = malloc( PLUGIN_PATH_LENGTH * sizeof(char) );

  // Get the name of the plugin's init function
  char* plugin_init_name = malloc( PLUGIN_PATH_LENGTH * sizeof(char) );
  snprintf(plugin_init_name, PLUGIN_PATH_LENGTH, "MDI_Plugin_open_%s", plugin_name);

  /*************************************************/
  /*************** BEGIN PLUGIN MODE ***************/
  /*************************************************/
  plugin_mode = 1;

  library_load_init(plugin_name, mpi_comm_ptr, libd, 1);
  if ( ret != 0 ) {
    free( plugin_path );
    free( plugin_init_name );
    return ret;
  }

  /*************************************************/
  /**************** END PLUGIN MODE ****************/
  /*************************************************/
  plugin_mode = 0;
//  current_code = driver_code_id;
  current_code = libd->shared_state->driver_code_id;

  // Delete the driver's communicator to the engine
  // This will also delete the engine code and its communicator
  //delete_communicator(driver_code_id, comm);

  // free memory from loading the plugin's initialization function
  free( plugin_path );
  free( plugin_init_name );

  *mdi_comm_ptr = comm;
  return 0;
}

int library_close_plugin(MDI_Comm mdi_comm) {
  code* this_code = get_code(current_code);
  communicator* this_comm = get_communicator(this_code->id, mdi_comm);
  library_data* libd = (library_data*) this_comm->method_data;

  if (libd->is_python == 0 ) {
  // Close the plugin library
#ifdef _WIN32
    FreeLibrary( libd->plugin_handle );
#else
    dlclose( libd->plugin_handle );
#endif
  }

  // Delete the driver's communicator to the engine
  // This will also delete the engine code and its communicator
  delete_communicator(current_code, mdi_comm);
  
  return 0;
}


/*! \brief Perform initialization of a communicator for library-based communication
 *
 */
int library_initialize() {
  code* this_code = get_code(current_code);

  MDI_Comm comm_id = new_communicator(this_code->id, MDI_LINK);
  communicator* new_comm = get_communicator(this_code->id, comm_id);
  new_comm->delete = communicator_delete_lib;
  new_comm->send = library_send;
  new_comm->recv = library_recv;

  // set the MDI version number of the new communicator
  new_comm->mdi_version[0] = MDI_MAJOR_VERSION;
  new_comm->mdi_version[1] = MDI_MINOR_VERSION;
  new_comm->mdi_version[2] = MDI_PATCH_VERSION;
  new_comm->name_length = MDI_NAME_LENGTH;
  new_comm->command_length = MDI_COMMAND_LENGTH;

  // allocate the method data
  library_data* libd = malloc(sizeof(library_data));
  //libd->connected_code = -1;
  //libd->buf_allocated = 0;
  libd->execute_on_send = 0;
  libd->mpi_comm = MPI_COMM_NULL;
  libd->plugin_options_allocated = 0;
  libd->plugin_options = NULL;
  libd->plugin_unedited_options = NULL;

  new_comm->method_data = libd;

  // if this is an engine, go ahead and set the driver as the connected code
  if ( strcmp(this_code->role, "ENGINE") == 0 ) {
    //int engine_code = current_code;
    //library_set_driver_current();
    //int driver_code_id = current_code;
    //libd->connected_code = driver_code_id;
    //current_code = engine_code;
    libd->shared_state = shared_state_from_driver;
    libd->shared_state->engine_mdi_comm = new_comm->id;
    libd->shared_state->delete_engine = library_delete_engine;
    libd->shared_state->lib_execute_command = library_execute_command;
    libd->shared_state->engine_code_id = current_code;
//    current_code = libd->shared_state->engine_code_id;

    // set the engine's mpi communicator
    //if ( plugin_mode ) {
      // get the driver's library data
      //code* driver_code = get_code(driver_code_id);
      //MDI_Comm matching_handle = library_get_matching_handle(comm_id);
      //communicator* driver_comm = get_communicator(driver_code->id, matching_handle);
      //library_data* driver_libd = (library_data*) driver_comm->method_data;



      //libd->mpi_comm = driver_libd->mpi_comm;
      libd->mpi_comm = *(MPI_Comm*)libd->shared_state->mpi_comm_ptr;
      this_code->intra_MPI_comm = libd->mpi_comm;

      // check whether MPI has been initialized
      int mpi_init_flag;
      if ( MPI_Initialized(&mpi_init_flag) ) {
        mdi_error("Error in MDI_Init: MPI_Initialized failed");
        return 1;
      }

      // Set the engine's MPI rank
      if ( mpi_init_flag == 1 ) {
        MPI_Comm_rank( this_code->intra_MPI_comm, &this_code->intra_rank );
      }
      else {
        this_code->intra_rank = 0;
      }
      
      libd->shared_state->intra_rank = this_code->intra_rank;
    //}

  }

  return new_comm->id;
}


/*! \brief Set the driver as the current code
 *
 */
int library_set_driver_current(MDI_Comm comm) {
  code* this_code = get_code(current_code);
  communicator* this_comm = get_communicator(current_code, comm);
  library_data* libd = (library_data*) this_comm->method_data;

  current_code = libd->shared_state->driver_code_id;

  /*
  // check if the current code is an ENGINE that is linked as a LIBRARY
  if ( strcmp(this_code->role, "ENGINE") == 0 ) {
    if ( this_code->is_library == 1 || this_code->is_library == 2 ) {
      // the calling code must actually be the driver, so update current_code
      int icode;
      int found_driver = 0;
      for ( icode = 0; icode < codes.size; icode++ ) {
        code* other_code = vector_get(&codes, icode);
        if ( strcmp(other_code->role, "DRIVER") == 0 ) {
//          current_code = icode;
          found_driver = 1;
        }
      }
      // confirm that the driver was found
      if ( found_driver == 0 ) {
        mdi_error("MDI_Accept_Communicator could not locate the driver; was MDI_Init called by the driver?");
        return 1;
      }
    }
  }
  */
  return 0;
}


/*! \brief Perform LIBRARY method operations upon a call to MDI_Accept_Communicator
 *
 */
int library_accept_communicator() {
  code* this_code = get_code(current_code);
  /*
  if ( this_code->called_set_execute_command_func && (! plugin_mode) ) {
    // library codes are not permitted to call MDI_Accept_communicator after calling
    // MDI_Set_execute_command_func, so assume that this call is being made by the driver
    library_set_driver_current();
  }
  */
  this_code = get_code(current_code);

  // if this is a DRIVER, check if there are any ENGINES that are linked to it
  if ( strcmp(this_code->role, "DRIVER") == 0 ) {

    int icode;
    int found_engine = 0;
    int iengine = 0;
    for ( icode = 0; icode < codes.size; icode++ ) {
      code* other_code = vector_get(&codes, icode);
      if ( strcmp(other_code->role, "ENGINE") == 0 ) {
	if ( other_code->is_library == 1 ) {
	  // flag that this library has connected to the driver
	  other_code->is_library = 2;

	  //iengine = icode;
	  iengine = other_code->id;
	  found_engine = 1;
	  break;
	}
      }
    }

    // create a new communicator for this engine
    if ( found_engine == 1 ) {
      int icomm = library_initialize();

      // set the connected code for the driver
      code* engine_code = get_code(iengine);
      communicator* this_comm = get_communicator(current_code, icomm);
      library_data* libd = (library_data*) this_comm->method_data;
      libd->connected_code = engine_code->id;
    }

  }

  return 0;
}


/*! \brief Get the handle to the matching communicator on the code to which comm points
 *
 * If running with MPI, this function must be called only by rank \p 0.
 * The function returns \p 0 on a success.
 *
 * \param [in]       comm
 *                   MDI communicator associated with the linked code.
 */
int library_get_matching_handle(MDI_Comm comm) {
  communicator* this = get_communicator(current_code, comm);

  // get the engine code to which this communicator connects
  library_data* libd = (library_data*) this->method_data;
  int iengine = libd->connected_code;
  code* engine_code = get_code(iengine);

  // identify the communicator on the engine that connects to the driver
  int icomm;
  int found_self = 0;
  int engine_comm_handle = 0;
  for ( icomm = 0; icomm < engine_code->comms->size; icomm++ ) {
    communicator* engine_comm = vector_get(engine_code->comms, icomm);
    library_data* engine_lib = (library_data*) engine_comm->method_data;
    if ( engine_lib->connected_code == current_code ) {
      found_self = 1;
      engine_comm_handle = engine_comm->id;
    }
  }

  // ensure that the communicator was found
  if ( found_self == 0 ) {
    mdi_error("Could not find communicator for engine; Did the engine call MDI_Init?"); 
    return 1;
  }

  return engine_comm_handle;
}


/*! \brief Set the next command that will be executed through the library communicator
 *
 * If running with MPI, this function must be called only by rank \p 0.
 * The function returns \p 0 on a success.
 *
 * \param [in]       command
 *                   Pointer to the command to be executed.
 * \param [in]       comm
 *                   MDI communicator associated with the intended recipient code.
 */
int library_set_command(const char* command, MDI_Comm comm) {
  communicator* this = get_communicator(current_code, comm);

  // get the engine code to which this communicator connects
  library_data* libd = (library_data*) this->method_data;
  //int iengine = libd->connected_code;

  // get the matching engine communicator
  //MDI_Comm engine_comm_handle = library_get_matching_handle(comm);
  //communicator* engine_comm = get_communicator(iengine, engine_comm_handle);

  // set the command
  //library_data* engine_lib = (library_data*) engine_comm->method_data;
  //snprintf(engine_lib->command, MDI_COMMAND_LENGTH_, "%s", command);
  snprintf(libd->shared_state->command, MDI_COMMAND_LENGTH_, "%s", command);

  return 0;
}


/*! \brief Execute a command through a communicator
 *
 * If running with MPI, this function must be called only by rank \p 0.
 * The function returns \p 0 on a success.
 *
 * \param [in]       command
 *                   Pointer to the command to be executed.
 * \param [in]       comm
 *                   MDI communicator associated with the intended recipient code.
 */
int library_execute_command(MDI_Comm comm) {
  int ret = 0;

  int idriver = current_code;
  communicator* this = get_communicator(current_code, comm);

  // get the engine code to which this communicator connects
  library_data* libd = (library_data*) this->method_data;
  //int iengine = libd->connected_code;
  //code* engine_code = get_code(iengine);

  //MDI_Comm engine_comm_handle = library_get_matching_handle(comm);
  MDI_Comm engine_comm_handle = libd->shared_state->engine_mdi_comm;
  //communicator* engine_comm = get_communicator(iengine, engine_comm_handle);
  //library_data* engine_lib = (library_data*) engine_comm->method_data;

  // set the current code to the engine
//  current_code = iengine;
  current_code = libd->shared_state->engine_code_id;

  // check if this command corresponds to one of MDI's standard built-in commands
  //int builtin_flag = general_builtin_command(engine_lib->command, engine_comm_handle);
  int builtin_flag = general_builtin_command(libd->shared_state->command, engine_comm_handle);
  //int builtin_flag = 0;

  if ( builtin_flag == 0 ) {
    // call execute_command now
    //void* class_obj = engine_code->execute_command_obj;
    void* class_obj = libd->shared_state->execute_command_obj;
    //ret = engine_code->execute_command(engine_lib->command,engine_comm_handle,class_obj);
    ret = libd->shared_state->execute_command(libd->shared_state->command, engine_comm_handle, class_obj);
  }

  // set the current code to the driver
//  current_code = idriver;
  current_code = libd->shared_state->driver_code_id;

  return ret;
}



/*! \brief Function to handle sending data through an MDI connection, using library-based communication
 *
 * \param [in]       buf
 *                   Pointer to the data to be sent.
 * \param [in]       count
 *                   Number of values (integers, double precision floats, characters, etc.) to be sent.
 * \param [in]       datatype
 *                   MDI handle (MDI_INT, MDI_DOUBLE, MDI_CHAR, etc.) corresponding to the type of data to be sent.
 * \param [in]       comm
 *                   MDI communicator associated with the intended recipient code.
 * \param [in]       msg_flag
 *                   Type of role this data has within a message.
 *                   0: Not part of a message.
 *                   1: The header of a message.
 *                   2: The body (data) of a message.
 */
int library_send(const void* buf, int count, MDI_Datatype datatype, MDI_Comm comm, int msg_flag) {
  int ret;

  code* this_code = get_code(current_code);
  communicator* this = get_communicator(current_code, comm);
  library_data* libd = (library_data*) this->method_data;

  //code* other_code = get_code(libd->connected_code);

  // get the rank of this process on the engine
  /*
  int engine_rank = 0;
  if ( this_code->is_library ) {
    engine_rank = this_code->intra_rank;
  }
  else {
    engine_rank = other_code->intra_rank;
  }
  */
  int engine_rank = libd->shared_state->intra_rank;

  // only send from rank 0
  if ( engine_rank == 0 ) {

    // determine the byte size of the data type being sent
    size_t datasize;
    MDI_Datatype basetype;
    ret = datatype_info(datatype, &datasize, &basetype);
    if ( ret != 0 ) {
      mdi_error("datatype_info returned nonzero value in library_send");
      return ret;
    }

    int nheader_actual = 4; // actual number of elements of nheader that were sent

    if ( msg_flag == 1 ) { // message header

      // confirm that libd->buf is not already allocated
      if ( libd->shared_state->buf_allocated != 0 ) {
        mdi_error("MDI recv buffer already allocated");
        return 1;
      }

      // get the size of the message body, based on the header information
      int* header = (int*)buf;
      int body_type = header[2];
      int body_size = header[3];

      // determine the byte size of the data type being sent in the body of the message
      size_t body_stride;
      ret = datatype_info(body_type, &body_stride, &basetype);
      if ( ret != 0 ) {
        mdi_error("datatype_info returned nonzero value in library_send");
        return ret;
      }

      int msg_bytes = ( (int)datasize * count ) + ( (int)body_stride * body_size );

      // allocate the memory required for the entire message
      libd->shared_state->buf = malloc( msg_bytes );
      libd->shared_state->buf_allocated = 1;

      // copy the header into libd->buf
      memcpy(libd->shared_state->buf, buf, nheader_actual * sizeof(int));

    }
    else if ( msg_flag == 2 ) { // message body

      // confirm that libd->buf has been allocated
      int has_header = 1;
      if ( libd->shared_state->buf_allocated == 0 ) {
        // libd->buf has not been allocated, which means there is no header
        has_header = 0;
        libd->shared_state->buf = malloc( datasize * count );
        libd->shared_state->buf_allocated = 1;
      }

      int offset = 0;
      if ( has_header == 1 ) {
        offset = nheader_actual * sizeof(int);
      }

      // copy the body into libd->buf
      memcpy((char*)libd->shared_state->buf + offset, buf, datasize * count);

    }
    else {

      mdi_error("MDI library unknown msg_flag in send\n");
      return 1;

    }

  }

  // check whether the recipient code should now execute its command
  if ( msg_flag == 2 && libd->execute_on_send ) {
    // have the recipient code execute its command
    //library_execute_command(comm);
    libd->shared_state->lib_execute_command(libd->shared_state->engine_mdi_comm);

    // turn off the execute_on_send flag
    libd->execute_on_send = 0;
  }

  return 0;
}



/*! \brief Function to handle receiving data through an MDI connection, using library-based communication
 *
 * \param [in]       buf
 *                   Pointer to the buffer where the received data will be stored.
 * \param [in]       count
 *                   Number of values (integers, double precision floats, characters, etc.) to be received.
 * \param [in]       datatype
 *                   MDI handle (MDI_INT, MDI_DOUBLE, MDI_CHAR, etc.) corresponding to the type of data to be received.
 * \param [in]       comm
 *                   MDI communicator associated with the connection to the sending code.
 * \param [in]       msg_flag
 *                   Type of role this data has within a message.
 *                   0: Not part of a message.
 *                   1: The header of a message.
 *                   2: The body (data) of a message.
 */
int library_recv(void* buf, int count, MDI_Datatype datatype, MDI_Comm comm, int msg_flag) {
  int ret;

  code* this_code = get_code(current_code);
  communicator* this = get_communicator(current_code, comm);
  library_data* libd = (library_data*) this->method_data;

  //code* other_code = get_code(libd->connected_code);
  //MDI_Comm other_comm_handle = library_get_matching_handle(comm);
  //communicator* other_comm = get_communicator(libd->connected_code, other_comm_handle);
  //library_data* other_lib = (library_data*) other_comm->method_data;

  // only recv from rank 0 of the engine
  /*
  int engine_rank = 0;
  if ( this_code->is_library ) {
    engine_rank = this_code->intra_rank;
  }
  else {
    engine_rank = other_code->intra_rank;
  }
  */
  int engine_rank = libd->shared_state->intra_rank;
  if ( engine_rank != 0 ) {
    return 0;
  }

  // determine the byte size of the data type being sent
  size_t datasize;
  MDI_Datatype basetype;
  ret = datatype_info(datatype, &datasize, &basetype);
  if ( ret != 0 ) {
    mdi_error("datatype_info returned nonzero value in library_recv");
    return ret;
  }

  // confirm that libd->buf is initialized
  if ( libd->shared_state->buf_allocated != 1 ) {
    mdi_error("MDI send buffer is not allocated");
    return 1;
  }

  // receive message header information
  // only do this if communicating with MDI version 1.1 or higher
  if ( ( this->mdi_version[0] > 1 ||
	 ( this->mdi_version[0] == 1 && this->mdi_version[1] >= 1 ) )
       && ipi_compatibility != 1 ) {

    if ( msg_flag == 1 ) { // message header

      memcpy(buf, libd->shared_state->buf, count * datasize);

    }
    else if ( msg_flag == 2 ) { // message body

      int nheader = 4;
      int offset = nheader * sizeof(int);
      memcpy(buf, (char*)libd->shared_state->buf + offset, count * datasize);

      // free the memory of libd->buf
      free( libd->shared_state->buf );
      libd->shared_state->buf_allocated = 0;

    }
    else {

      mdi_error("MDI library unknown msg_flag in send\n");
      return 1;

    }

  }

  return 0;
}



/*! \brief Function for LIBRARY-specific deletion operations for communicator deletion
 */
int communicator_delete_lib(void* comm) {
  communicator* this_comm = (communicator*) comm;
  code* this_code = get_code(this_comm->code_id);
  library_data* libd = (library_data*) this_comm->method_data;

  // if this is the driver, delete the engine code
  if ( this_code->is_library == 0 ) {
    //delete_code(libd->connected_code);
    libd->shared_state->delete_engine( libd->shared_state->engine_code_id );

    if ( libd->shared_state->plugin_argv_allocated ) {
      free( libd->shared_state->plugin_argv );
    }

    // delete the method-specific information
    if ( libd->shared_state->buf_allocated ) {
      free( libd->shared_state->buf );
    }

    // delete the shared state, but only on the driver side
    free( libd->shared_state );

  }

  if ( libd->plugin_options_allocated ) {
    free( libd->plugin_options );
    free( libd->plugin_unedited_options );
  }

  free( libd );

  return 0;
}


/*! \brief Function to delete all of the engine's state
 */
int library_delete_engine(int code_id) {
  delete_code(code_id);
  if ( codes.size == 0 ) {
    free(codes.data);
  }
  //free(methods.data);
  return 0;
}



/*! \brief Function to set the plugin's state
 */
int library_set_state(void* state) {
  shared_state_from_driver = (plugin_shared_state*) state;

  // activate plugin mode
  plugin_mode = 1;
  //is_initialized = 1;

  

  MDI_Init(&shared_state_from_driver->plugin_argc, &shared_state_from_driver->plugin_argv);

  return 0;
}
