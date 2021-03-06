#include "actions.h"

#include <cbdebugger_interfaces.h>
#include <cbplugin.h>
#include <logmanager.h>

#include "cmd_result_parser.h"
#include "frame.h"
#include "updated_variable.h"

namespace dbg_mi
{

void BreakpointAddAction::OnStart()
{
    wxString cmd(wxT("-break-insert -f "));

    if(m_breakpoint->HasCondition())
        cmd += wxT("-c ") + m_breakpoint->GetCondition() + wxT(" ");
    if(m_breakpoint->HasIgnoreCount())
        cmd += wxT("-i ") + wxString::Format(wxT("%d "), m_breakpoint->GetIgnoreCount());

    cmd += wxString::Format(wxT("%s:%d"), m_breakpoint->GetLocation().c_str(), m_breakpoint->GetLine());
    m_initial_cmd = Execute(cmd);
    m_logger.Debug(wxT("BreakpointAddAction::m_initial_cmd = ") + m_initial_cmd.ToString());
}

void BreakpointAddAction::OnCommandOutput(CommandID const &id, ResultParser const &result)
{
    m_logger.Debug(wxT("BreakpointAddAction::OnCommandResult: ") + id.ToString());

    if(m_initial_cmd == id)
    {
        bool finish = true;
        const ResultValue &value = result.GetResultValue();
        if (result.GetResultClass() == ResultParser::ClassDone)
        {
            const ResultValue *number = value.GetTupleValue(wxT("bkpt.number"));
            if(number)
            {
                const wxString &number_value = number->GetSimpleValue();
                long n;
                if(number_value.ToLong(&n, 10))
                {
                    m_logger.Debug(wxString::Format(wxT("BreakpointAddAction::breakpoint index is %d"), n));
                    m_breakpoint->SetIndex(n);

                    if(!m_breakpoint->IsEnabled())
                    {
                        m_disable_cmd = Execute(wxString::Format(wxT("-break-disable %d"), n));
                        finish = false;
                    }
                }
                else
                    m_logger.Debug(wxT("BreakpointAddAction::error getting the index :( "));
            }
            else
            {
                m_logger.Debug(wxT("BreakpointAddAction::error getting number value:( "));
                m_logger.Debug(value.MakeDebugString());
            }
        }
        else if (result.GetResultClass() == ResultParser::ClassError)
        {
            wxString message;
            if (Lookup(value, wxT("msg"), message))
                m_logger.Log(message, Logger::Log::Error);
        }

        if (finish)
        {
            m_logger.Debug(wxT("BreakpointAddAction::Finishing1"));
            Finish();
        }
    }
    else if(m_disable_cmd == id)
    {
        m_logger.Debug(wxT("BreakpointAddAction::Finishing2"));
        Finish();
    }
}

GenerateBacktrace::GenerateBacktrace(SwitchToFrameInvoker *switch_to_frame, BacktraceContainer &backtrace,
                                     CurrentFrame &current_frame, Logger &logger) :
    m_switch_to_frame(switch_to_frame),
    m_backtrace(backtrace),
    m_logger(logger),
    m_current_frame(current_frame),
    m_first_valid(-1),
    m_old_active_frame(-1),
    m_parsed_backtrace(false),
    m_parsed_args(false),
    m_parsed_frame_info(false)
{
}

GenerateBacktrace::~GenerateBacktrace()
{
    delete m_switch_to_frame;
}

void GenerateBacktrace::OnCommandOutput(CommandID const &id, ResultParser const &result)
{
    if(id == m_backtrace_id)
    {
        ResultValue const *stack = result.GetResultValue().GetTupleValue(wxT("stack"));
        if(!stack)
            m_logger.Debug(wxT("GenerateBacktrace::OnCommandOutput: no stack tuple in the output"));
        else
        {
            int size = stack->GetTupleSize();
            m_logger.Debug(wxString::Format(wxT("GenerateBacktrace::OnCommandOutput: tuple size %d %s"),
                                            size, stack->MakeDebugString().c_str()));

            m_backtrace.clear();

            for(int ii = 0; ii < size; ++ii)
            {
                ResultValue const *frame_value = stack->GetTupleValueByIndex(ii);
                assert(frame_value);
                Frame frame;
                if(frame.ParseFrame(*frame_value))
                {
                    cbStackFrame s;
                    if(frame.HasValidSource())
                        s.SetFile(frame.GetFilename(), wxString::Format(wxT("%d"), frame.GetLine()));
                    else
                        s.SetFile(frame.GetFrom(), wxEmptyString);
                    s.SetSymbol(frame.GetFunction());
                    s.SetNumber(ii);
                    s.SetAddress(frame.GetAddress());
                    s.MakeValid(frame.HasValidSource());
                    if(s.IsValid() && m_first_valid == -1)
                        m_first_valid = ii;

                    m_backtrace.push_back(cb::shared_ptr<cbStackFrame>(new cbStackFrame(s)));
                }
                else
                    m_logger.Debug(wxT("can't parse frame: ") + frame_value->MakeDebugString());
            }
        }
        m_parsed_backtrace = true;
    }
    else if(id == m_args_id)
    {
        m_logger.Debug(wxT("GenerateBacktrace::OnCommandOutput arguments"));
        FrameArguments arguments;

        if(!arguments.Attach(result.GetResultValue()))
        {
            m_logger.Debug(wxT("GenerateBacktrace::OnCommandOutput: can't attach to output of command: ")
                           + id.ToString());
        }
        else if(arguments.GetCount() != static_cast<int>(m_backtrace.size()))
        {
            m_logger.Debug(wxT("GenerateBacktrace::OnCommandOutput: stack arg count differ from the number of frames"));
        }
        else
        {
            int size = arguments.GetCount();
            for(int ii = 0; ii < size; ++ii)
            {
                wxString args;
                if(arguments.GetFrame(ii, args))
                    m_backtrace[ii]->SetSymbol(m_backtrace[ii]->GetSymbol() + wxT("(") + args + wxT(")"));
                else
                {
                    m_logger.Debug(wxString::Format(wxT("GenerateBacktrace::OnCommandOutput: ")
                                                    wxT("can't get args for frame %d"),
                                                    ii));
                }
            }
        }
        m_parsed_args = true;
    }
    else if (id == m_frame_info_id)
    {
        m_parsed_frame_info = true;

        //^done,frame={level="0",addr="0x0000000000401060",func="main",
        //file="/path/main.cpp",fullname="/path/main.cpp",line="80"}
        if (result.GetResultClass() != ResultParser::ClassDone)
        {
            m_old_active_frame = 0;
            m_logger.Debug(wxT("Wrong result class, using default value!"));
        }
        else
        {
            if (!Lookup(result.GetResultValue(), wxT("frame.level"), m_old_active_frame))
                m_old_active_frame = 0;
        }
    }

    if(m_parsed_backtrace && m_parsed_args && m_parsed_frame_info)
    {
        if (!m_backtrace.empty())
        {
            int frame = m_current_frame.GetUserSelectedFrame();
            if (frame < 0 && cbDebuggerCommonConfig::GetFlag(cbDebuggerCommonConfig::AutoSwitchFrame))
                frame = m_first_valid;
            if (frame < 0)
                frame = 0;

            m_current_frame.SetFrame(frame);
            int number = m_backtrace.empty() ? 0 : m_backtrace[frame]->GetNumber();
            if (m_old_active_frame != number)
                m_switch_to_frame->Invoke(number);
        }

        Manager::Get()->GetDebuggerManager()->GetBacktraceDialog()->Reload();
        Finish();
    }
}
void GenerateBacktrace::OnStart()
{
    m_frame_info_id = Execute(wxT("-stack-info-frame"));
    m_backtrace_id = Execute(wxT("-stack-list-frames 0 30"));
    m_args_id = Execute(wxT("-stack-list-arguments 1 0 30"));
}

GenerateThreadsList::GenerateThreadsList(ThreadsContainer &threads, int current_thread_id, Logger &logger) :
    m_threads(threads),
    m_logger(logger),
    m_current_thread_id(current_thread_id)
{
}

void GenerateThreadsList::OnCommandOutput(CommandID const & /*id*/, ResultParser const &result)
{
    Finish();
    m_threads.clear();

    int current_thread_id = 0;
    if(!Lookup(result.GetResultValue(), wxT("current-thread-id"), current_thread_id))
    {
        m_logger.Debug(wxT("GenerateThreadsList::OnCommandOutput - no current thread id"));
        return;
    }

    ResultValue const *threads = result.GetResultValue().GetTupleValue(wxT("threads"));
    if(!threads || (threads->GetType() != ResultValue::Tuple && threads->GetType() != ResultValue::Array))
    {
        m_logger.Debug(wxT("GenerateThreadsList::OnCommandOutput - no threads"));
        return;
    }
    int count = threads->GetTupleSize();
    for(int ii = 0; ii < count; ++ii)
    {
        ResultValue const &thread_value = *threads->GetTupleValueByIndex(ii);

        int thread_id;
        if(!Lookup(thread_value, wxT("id"), thread_id))
            continue;

        wxString info;
        if(!Lookup(thread_value, wxT("target-id"), info))
            info = wxEmptyString;

        ResultValue const *frame_value = thread_value.GetTupleValue(wxT("frame"));

        if(frame_value)
        {
            wxString str;

            if(Lookup(*frame_value, wxT("addr"), str))
                info += wxT(" ") + str;
            if(Lookup(*frame_value, wxT("func"), str))
            {
                info += wxT(" ") + str;

                if(FrameArguments::ParseFrame(*frame_value, str))
                    info += wxT("(") + str + wxT(")");
                else
                    info += wxT("()");
            }

            int line;

            if(Lookup(*frame_value, wxT("file"), str) && Lookup(*frame_value, wxT("line"), line))
            {
                info += wxString::Format(wxT(" in %s:%d"), str.c_str(), line);
            }
            else if(Lookup(*frame_value, wxT("from"), str))
                info += wxT(" in ") + str;
        }

        m_threads.push_back(cb::shared_ptr<cbThread>(new cbThread(thread_id == current_thread_id, thread_id, info)));
    }

    Manager::Get()->GetDebuggerManager()->GetThreadsDialog()->Reload();
}

void GenerateThreadsList::OnStart()
{
    Execute(wxT("-thread-info"));
}

void ParseWatchInfo(ResultValue const &value, int &children_count, bool &dynamic, bool &has_more)
{
    dynamic = has_more = false;

    int temp;
    if (Lookup(value, wxT("dynamic"), temp))
        dynamic = (temp == 1);
    if (Lookup(value, wxT("has_more"), temp))
        has_more = (temp == 1);

    if(!Lookup(value, wxT("numchild"), children_count))
        children_count = -1;
}

void ParseWatchValueID(Watch &watch, ResultValue const &value)
{
    wxString s;
    if(Lookup(value, wxT("name"), s))
        watch.SetID(s);

    if(Lookup(value, wxT("value"), s))
        watch.SetValue(s);

    if(Lookup(value, wxT("type"), s))
        watch.SetType(s);
}

bool WatchHasType(ResultValue const &value)
{
    wxString s;
    return Lookup(value, wxT("type"), s);
}

void AppendNullChild(cb::shared_ptr<Watch> watch)
{
    cbWatch::AddChild(watch, cb::shared_ptr<cbWatch>(new Watch(wxT("updating..."), watch->ForTooltip())));
}

cb::shared_ptr<Watch> AddChild(cb::shared_ptr<Watch> parent, ResultValue const &child_value, wxString const &symbol,
                               WatchesContainer &watches)
{
    wxString id;
    if(!Lookup(child_value, wxT("name"), id))
        return cb::shared_ptr<Watch>();

    cb::shared_ptr<Watch> child = FindWatch(id, watches);
    if(child)
    {
        wxString s;
        if(Lookup(child_value, wxT("value"), s))
            child->SetValue(s);

        if(Lookup(child_value, wxT("type"), s))
            child->SetType(s);
    }
    else
    {
        child = cb::shared_ptr<Watch>(new Watch(symbol, parent->ForTooltip()));
        ParseWatchValueID(*child, child_value);
        cbWatch::AddChild(parent, child);
    }

    child->MarkAsRemoved(false);
    return child;
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void UpdateWatches(Logger &logger)
{
#ifndef TEST_PROJECT
    logger.Debug(wxT("updating watches"));
    Manager::Get()->GetDebuggerManager()->GetWatchesDialog()->UpdateWatches();
#endif
}

void UpdateWatchesTooltipOrAll(const cb::shared_ptr<Watch> &watch, Logger &logger)
{
#ifndef TEST_PROJECT
    if (watch->ForTooltip())
    {
        logger.Debug(wxT("updating tooltip watch"));
        Manager::Get()->GetDebuggerManager()->GetInterfaceFactory()->UpdateValueTooltip();
    }
    else
        UpdateWatches(logger);
#endif
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
WatchBaseAction::WatchBaseAction(WatchesContainer &watches, Logger &logger) :
    m_watches(watches),
    m_logger(logger),
    m_sub_commands_left(0),
    m_start(-1),
    m_end(-1)
{
}

WatchBaseAction::~WatchBaseAction()
{
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
bool WatchBaseAction::ParseListCommand(CommandID const &id, ResultValue const &value)
{
    bool error = false;
    m_logger.Debug(wxT("WatchBaseAction::ParseListCommand - steplistchildren for id: ")
                   + id.ToString() + wxT(" -> ") + value.MakeDebugString());

    ListCommandParentMap::iterator it = m_parent_map.find(id);
    if(it == m_parent_map.end() || !it->second)
    {
        m_logger.Debug(wxT("WatchBaseAction::ParseListCommand - no parent for id: ") + id.ToString());
        return false;
    }

    struct DisplayHint
    {
        enum Enum { None=0, Array, Map };
    };

    DisplayHint::Enum displayHint = DisplayHint::None;

    wxString strDisplayHint;
    if (Lookup(value, wxT("displayhint"), strDisplayHint))
    {
        if (strDisplayHint == wxT("map"))
            displayHint = DisplayHint::Map;
        else if (strDisplayHint == wxT("array"))
            displayHint = DisplayHint::Array;
    }

    ResultValue const *children = value.GetTupleValue(wxT("children"));
    if(children)
    {
        int count = children->GetTupleSize();

        m_logger.Debug(wxString::Format(wxT("WatchBaseAction::ParseListCommand - children %d"), count));
        cb::shared_ptr<Watch> parent_watch = it->second;

        wxString strMapKey;

        for(int ii = 0; ii < count; ++ii)
        {
            ResultValue const *child_value;
            child_value = children->GetTupleValueByIndex(ii);

            if(child_value->GetName() == wxT("child"))
            {
                wxString symbol;
                if(!Lookup(*child_value, wxT("exp"), symbol))
                    symbol = wxT("--unknown--");

                cb::shared_ptr<Watch> child;
                bool dynamic, has_more;

                int children_count;
                ParseWatchInfo(*child_value, children_count, dynamic, has_more);

                bool mapValue = false;

                if (displayHint == DisplayHint::Map)
                {
                    if ((ii & 1) == 0)
                    {
                        if (!Lookup(*child_value, wxT("value"), strMapKey))
                            strMapKey = wxEmptyString;
                        continue;
                    }
                    else
                        mapValue = true;
                }

                if(dynamic && has_more)
                {
                    child = cb::shared_ptr<Watch>(new Watch(symbol, parent_watch->ForTooltip(), false));
                    ParseWatchValueID(*child, *child_value);
                    ExecuteListCommand(child, parent_watch);
                }
                else
                {
                    switch(children_count)
                    {
                    case -1:
                        error = true;
                        break;
                    case 0:
                        if(!parent_watch->HasBeenExpanded())
                        {
                            parent_watch->SetHasBeenExpanded(true);
                            parent_watch->RemoveChildren();
                        }
                        child = AddChild(parent_watch, *child_value, (mapValue ? strMapKey : symbol), m_watches);
                        if (dynamic)
                        {
                            child->SetDeleteOnCollapse(false);
                            wxString id;
                            if(Lookup(*child_value, wxT("name"), id))
                                ExecuteListCommand(id, child);
                        }
                        child = cb::shared_ptr<Watch>();
                        break;
                    default:
                        if(WatchHasType(*child_value))
                        {
                            if(!parent_watch->HasBeenExpanded())
                            {
                                parent_watch->SetHasBeenExpanded(true);
                                parent_watch->RemoveChildren();
                            }
                            child = AddChild(parent_watch, *child_value, (mapValue ? strMapKey : symbol), m_watches);
                            AppendNullChild(child);

                            m_logger.Debug(wxT("WatchBaseAction::ParseListCommand - adding child ")
                                           + child->GetDebugString()
                                           + wxT(" to ") + parent_watch->GetDebugString());
                            child = cb::shared_ptr<Watch>();
                        }
                        else
                        {
                            wxString id;
                            if(Lookup(*child_value, wxT("name"), id))
                                ExecuteListCommand(id, parent_watch);
                        }
                    }
                }
            }
            else
            {
                m_logger.Debug(wxT("WatchBaseAction::ParseListCommand - can't find child in ")
                               + children->GetTupleValueByIndex(ii)->MakeDebugString());
            }
        }
        parent_watch->RemoveMarkedChildren();
    }
    return !error;
}

void WatchBaseAction::ExecuteListCommand(cb::shared_ptr<Watch> watch, cb::shared_ptr<Watch> parent)
{
    CommandID id;

    if(m_start > -1 && m_end > -1)
    {
        id = Execute(wxString::Format(wxT("-var-list-children 2 \"%s\" %d %d "),
                                      watch->GetID().c_str(), m_start, m_end));
    }
    else
        id = Execute(wxString::Format(wxT("-var-list-children 2 \"%s\""), watch->GetID().c_str()));

    m_parent_map[id] = parent ? parent : watch;
    ++m_sub_commands_left;
}

void WatchBaseAction::ExecuteListCommand(wxString const &watch_id, cb::shared_ptr<Watch> parent)
{
    if (!parent)
    {
        m_logger.Debug(wxT("Parent for '") + watch_id + wxT("' is NULL; skipping this watch"));
        return;
    }
    CommandID id;

    if(m_start > -1 && m_end > -1)
        id = Execute(wxString::Format(wxT("-var-list-children 2 \"%s\" %d %d "), watch_id.c_str(), m_start, m_end));
    else
        id = Execute(wxString::Format(wxT("-var-list-children 2 \"%s\""), watch_id.c_str()));

    m_parent_map[id] = parent;
    ++m_sub_commands_left;
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
WatchCreateAction::WatchCreateAction(cb::shared_ptr<Watch> const &watch, WatchesContainer &watches, Logger &logger) :
    WatchBaseAction(watches, logger),
    m_watch(watch),
    m_step(StepCreate)
{
}

void WatchCreateAction::OnCommandOutput(CommandID const &id, ResultParser const &result)
{
    --m_sub_commands_left;
    m_logger.Debug(wxT("WatchCreateAction::OnCommandOutput - processing command ") + id.ToString());
    bool error = false;
    if(result.GetResultClass() == ResultParser::ClassDone)
    {
        ResultValue const &value = result.GetResultValue();
        switch(m_step)
        {
        case StepCreate:
            {
                bool dynamic, has_more;
                int children;
                ParseWatchInfo(value, children, dynamic, has_more);
                ParseWatchValueID(*m_watch, value);
                if(dynamic && has_more)
                {
                    m_step = StepSetRange;
                    Execute(wxT("-var-set-update-range \"") + m_watch->GetID() + wxT("\" 0 100"));
                    AppendNullChild(m_watch);

                }
                else if(children > 0)
                {
                    m_step = StepListChildren;
                    AppendNullChild(m_watch);
                }
                else
                    Finish();
            }
            break;
        case StepListChildren:
            error = !ParseListCommand(id, value);
            break;
        }
    }
    else
    {
        if(result.GetResultClass() == ResultParser::ClassError)
            m_watch->SetValue(wxT("The expression can't be evaluated"));
        error = true;
    }

    if(error)
    {
        m_logger.Debug(wxT("WatchCreateAction::OnCommandOutput - error in command: ") + id.ToString());
        UpdateWatches(m_logger);
        Finish();
    }
    else if(m_sub_commands_left == 0)
    {
        m_logger.Debug(wxT("WatchCreateAction::Output - finishing at") + id.ToString());
        UpdateWatches(m_logger);
        Finish();
    }
}

void WatchCreateAction::OnStart()
{
    wxString symbol;
    m_watch->GetSymbol(symbol);
    symbol.Replace(wxT("\""), wxT("\\\""));
    Execute(wxString::Format(wxT("-var-create - @ \"%s\""), symbol.c_str()));
    m_sub_commands_left = 1;
}
///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
WatchCreateTooltipAction::~WatchCreateTooltipAction()
{
    if (m_watch->ForTooltip())
        Manager::Get()->GetDebuggerManager()->GetInterfaceFactory()->ShowValueTooltip(m_watch, m_rect);
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
WatchesUpdateAction::WatchesUpdateAction(WatchesContainer &watches, Logger &logger) :
    WatchBaseAction(watches, logger)
{
}

void WatchesUpdateAction::OnStart()
{
    m_update_command = Execute(wxT("-var-update 1 *"));
    m_sub_commands_left = 1;
}

bool WatchesUpdateAction::ParseUpdate(ResultParser const &result)
{
    if(result.GetResultClass() == ResultParser::ClassError)
    {
        Finish();
        return false;
    }
    ResultValue const *list = result.GetResultValue().GetTupleValue(wxT("changelist"));
    if(list)
    {
        int count = list->GetTupleSize();
        for(int ii = 0; ii < count; ++ii)
        {
            ResultValue const *value = list->GetTupleValueByIndex(ii);

            wxString expression;
            if(!Lookup(*value, wxT("name"), expression))
            {
                m_logger.Debug(wxT("WatchesUpdateAction::Output - no name in ") + value->MakeDebugString());
                continue;
            }

            cb::shared_ptr<Watch> watch = FindWatch(expression, m_watches);
            if(!watch)
            {
                m_logger.Debug(wxT("WatchesUpdateAction::Output - can't find watch ") + expression);
                continue;
            }

            UpdatedVariable updated_var;
            if(updated_var.Parse(*value))
            {
                switch(updated_var.GetInScope())
                {
                case UpdatedVariable::InScope_No:
                    watch->Expand(false);
                    watch->RemoveChildren();
                    watch->SetValue(wxT("-- not in scope --"));
                    break;
                case UpdatedVariable::InScope_Invalid:
                    watch->Expand(false);
                    watch->RemoveChildren();
                    watch->SetValue(wxT("-- invalid -- "));
                    break;
                case UpdatedVariable::InScope_Yes:
                    if(updated_var.IsDynamic())
                    {
                        if(updated_var.HasNewNumberOfChildren())
                        {
                            watch->RemoveChildren();

                            if(updated_var.GetNewNumberOfChildren() > 0)
                                ExecuteListCommand(watch);
                        }
                        else if(updated_var.HasMore())
                        {
                            watch->MarkChildsAsRemoved(); // watch->RemoveChildren();
                            ExecuteListCommand(watch);
                        }
                        else if(updated_var.HasValue())
                        {
                            watch->SetValue(updated_var.GetValue());
                            watch->MarkAsChanged(true);
                        }
                        else
                        {
                            m_logger.Debug(wxT("WatchesUpdateAction::Output - unhandled dynamic variable"));
                            m_logger.Debug(wxT("WatchesUpdateAction::Output - ") + updated_var.MakeDebugString());
                        }
                    }
                    else
                    {
                        if(updated_var.HasNewNumberOfChildren())
                        {
                            watch->RemoveChildren();

                            if(updated_var.GetNewNumberOfChildren() > 0)
                                ExecuteListCommand(watch);
                        }
                        if(updated_var.HasValue())
                        {
                            watch->SetValue(updated_var.GetValue());
                            watch->MarkAsChanged(true);
                            m_logger.Debug(wxT("WatchesUpdateAction::Output - ")
                                           + expression + wxT(" = ") + updated_var.GetValue());
                        }
                        else
                        {
                            watch->SetValue(wxEmptyString);
                        }
                    }
                    break;
                }
            }

        }
    }
    return true;
}

void WatchesUpdateAction::OnCommandOutput(CommandID const &id, ResultParser const &result)
{
    --m_sub_commands_left;

    if(id == m_update_command)
    {
        for(WatchesContainer::iterator it = m_watches.begin();  it != m_watches.end(); ++it)
            (*it)->MarkAsChangedRecursive(false);

        if(!ParseUpdate(result))
        {
            Finish();
            return;
        }
    }
    else
    {
        ResultValue const &value = result.GetResultValue();
        if(!ParseListCommand(id, value))
        {
            m_logger.Debug(wxT("WatchUpdateAction::Output - ParseListCommand failed ") + id.ToString());
            Finish();
            return;
        }
    }

    if(m_sub_commands_left == 0)
    {
        m_logger.Debug(wxT("WatchUpdateAction::Output - finishing at") + id.ToString());
        UpdateWatches(m_logger);
        Finish();
    }
}

void WatchExpandedAction::OnStart()
{
    m_update_id = Execute(wxT("-var-update ") + m_expanded_watch->GetID());
    ExecuteListCommand(m_expanded_watch, cb::shared_ptr<Watch>());
}

void WatchExpandedAction::OnCommandOutput(CommandID const &id, ResultParser const &result)
{
    if (id == m_update_id)
        return;

    --m_sub_commands_left;
    m_logger.Debug(wxT("WatchExpandedAction::Output - ") + result.GetResultValue().MakeDebugString());
    if(!ParseListCommand(id, result.GetResultValue()))
    {
        m_logger.Debug(wxT("WatchExpandedAction::Output - error in command ") + id.ToString());
        // Update the watches even if there is an error, so some partial information can be displayed.
        UpdateWatchesTooltipOrAll(m_expanded_watch, m_logger);
        Finish();
    }
    else if(m_sub_commands_left == 0)
    {
        m_logger.Debug(wxT("WatchExpandedAction::Output - done"));
        UpdateWatchesTooltipOrAll(m_expanded_watch, m_logger);
        Finish();
    }
}

///////////////////////////////////////////////////////////////////////////////////////////////////////////////////////
void WatchCollapseAction::OnStart()
{
    Execute(wxT("-var-delete -c ") + m_collapsed_watch->GetID());
}

void WatchCollapseAction::OnCommandOutput(CommandID const &id, ResultParser const &result)
{
    if(result.GetResultClass() == ResultParser::ClassDone)
    {
        m_collapsed_watch->SetHasBeenExpanded(false);
        m_collapsed_watch->RemoveChildren();
        AppendNullChild(m_collapsed_watch);
        UpdateWatchesTooltipOrAll(m_collapsed_watch, m_logger);
    }
    Finish();
}

} // namespace dbg_mi
