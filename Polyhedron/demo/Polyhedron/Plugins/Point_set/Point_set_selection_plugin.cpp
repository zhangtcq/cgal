#include <CGAL/Exact_predicates_inexact_constructions_kernel.h>
#include <CGAL/Bbox_2.h>
#include <QtCore/qglobal.h>
#include <CGAL/Qt/manipulatedCameraFrame.h>
#include <CGAL/Search_traits_3.h>
#include <CGAL/Orthogonal_k_neighbor_search.h>
#include <CGAL/Search_traits_3.h>
#include <CGAL/Search_traits_adapter.h>
#include <CGAL/Fuzzy_sphere.h>
#include <CGAL/linear_least_squares_fitting_3.h>


#include "Messages_interface.h"
#include "Scene_points_with_normal_item.h"
#include "Scene_polylines_item.h"

#include <CGAL/Three/Scene_interface.h>
#include <CGAL/Three/Polyhedron_demo_plugin_helper.h>
#include <CGAL/Three/Three.h>
#include "ui_Point_set_selection_widget.h"
#include <Plugins/PCA/Scene_edit_box_item.h>
#include "Point_set_3.h"

#include <QAction>
#include <QMainWindow>
#include <QApplication>

#include <QEvent>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QMessageBox>

#include <QMultipleInputDialog.h>
#include <QDoubleSpinBox>

#include <map>
#include <fstream>
#include <boost/make_shared.hpp>
#include "Selection_visualizer.h"

//#undef  CGAL_LINKED_WITH_TBB
#ifdef CGAL_LINKED_WITH_TBB
#include <tbb/parallel_for.h>
#include <tbb/mutex.h>
#include <tbb/blocked_range.h>
#include <tbb/scalable_allocator.h>  
#endif // CGAL_LINKED_WITH_TBB


///////////////////////////////////////////////////////////////////////////////////////////////////


class Selection_test {

  Point_set* point_set;
  bool* selected;
  CGAL::qglviewer::Camera* camera;
  const CGAL::qglviewer::Vec offset;
  Scene_edit_box_item* edit_box;
  Selection_visualizer* visualizer;
  const Ui::PointSetSelection& ui_widget;

  
public:
  
  Selection_test(Point_set* point_set,
                 bool* selected,
                 CGAL::qglviewer::Camera* camera,
                 const CGAL::qglviewer::Vec offset,
                 Scene_edit_box_item* edit_box,
                 Selection_visualizer* visualizer,
                 const Ui::PointSetSelection& ui_widget)
    : point_set (point_set)
    , selected (selected)
    , camera (camera)
    , offset (offset)
    , edit_box (edit_box)
    , visualizer (visualizer)
    , ui_widget (ui_widget)
  {
  }

#ifdef CGAL_LINKED_WITH_TBB
  void operator()(const tbb::blocked_range<std::size_t>& r) const
  {
    for( std::size_t i = r.begin(); i != r.end(); ++i)
      apply (i);
  }
#endif // CGAL_LINKED_WITH_TBB

  void apply (std::size_t i) const
  {
    Point_set::Index idx = *(point_set->begin() + i);

    const Kernel::Point_3& p = point_set->point (idx);
    CGAL::qglviewer::Vec vp (p.x (), p.y (), p.z ());
    bool now_selected = false;
    if(!ui_widget.box->isChecked())
    {
      CGAL::qglviewer::Vec vsp = camera->projectedCoordinatesOf (vp + offset);
      now_selected = visualizer->is_selected (vsp);
    }
    else if(edit_box)
    {
      if(p.x() >= edit_box->point(0,0) - offset.x &&
         p.y() >= edit_box->point(0,1) - offset.y &&
         p.z() <= edit_box->point(0,2) - offset.z &&
         p.x() <= edit_box->point(6,0) - offset.x &&
         p.y() <= edit_box->point(6,1) - offset.y &&
         p.z() >= edit_box->point(6,2) - offset.z
        )
      {
        now_selected = true;
      }
    }

    if (ui_widget.new_selection->isChecked())
      selected[idx] = now_selected;
    else
    {
      bool already_selected = point_set->is_selected (point_set->begin() + i);
      if (ui_widget.union_selection->isChecked())
        selected[idx] = (already_selected || now_selected);
      else if (ui_widget.intersection->isChecked())
        selected[idx] = (already_selected && now_selected);
      else if (ui_widget.diff->isChecked())
        selected[idx] = (already_selected && !now_selected);
    }
  }
};


class Neighborhood
{
  typedef Kernel Geom_traits;

  typedef CGAL::Search_traits_3<Geom_traits> SearchTraits_3;
  typedef CGAL::Search_traits_adapter <Point_set::Index, Point_set::Point_map, SearchTraits_3> Search_traits;
  typedef CGAL::Orthogonal_k_neighbor_search<Search_traits> Neighbor_search;
  typedef Neighbor_search::Tree Tree;
  typedef Neighbor_search::Distance Distance;
  typedef CGAL::Fuzzy_sphere<Search_traits> Sphere;

  Scene_points_with_normal_item* points_item;
  boost::shared_ptr<Tree> tree;
  
public:

  Neighborhood () : points_item (NULL)
  {
  }

  ~Neighborhood ()
  {

  }

  Neighborhood& point_set (Scene_points_with_normal_item* points_item)
  {
    if (this->points_item != points_item)
    {
      this->points_item = points_item;
      
      tree = boost::make_shared<Tree> (points_item->point_set()->begin(),
                                       points_item->point_set()->end(),
                                       Tree::Splitter(),
                                       Search_traits (points_item->point_set()->point_map()));

    }
    return *this;
  }

  void expand()
  {
    if (points_item->point_set()->nb_selected_points() == 0)
      return;

    Distance tr_dist(points_item->point_set()->point_map());
    
    std::vector<bool> selected_bitmap (points_item->point_set()->size(), false);

    for (Point_set::iterator it = points_item->point_set()->first_selected();
         it != points_item->point_set()->end(); ++ it)
    {
      Neighbor_search search(*tree, points_item->point_set()->point(*it), 6, 0, true, tr_dist);
      for (Neighbor_search::iterator nit = search.begin(); nit != search.end(); ++ nit)
        selected_bitmap[nit->first] = true;
    }

    points_item->point_set()->set_first_selected
      (std::partition (points_item->point_set()->begin(), points_item->point_set()->end(),
                       [&] (const Point_set::Index& idx) -> bool
                       { return !selected_bitmap[idx]; }));

    points_item->invalidateOpenGLBuffers();
    points_item->itemChanged();
  }

  void reduce()
  {
    if (points_item->point_set()->nb_selected_points() == 0)
      return;

    points_item->invertSelection();
    expand();
    points_item->invertSelection();
    
    points_item->invalidateOpenGLBuffers();
    points_item->itemChanged();
  }

  void grow_region (const Kernel::Point_3& query, double epsilon, double cluster_epsilon,
                    unsigned int normal_threshold)
  {
    double cos_threshold = std::cos (CGAL_PI * normal_threshold / 180.);

    Distance tr_dist(points_item->point_set()->point_map());
 
    std::set<Point_set::Index> index_container;
    std::vector<Point_set::Index> index_container_former_ring;
    std::set<Point_set::Index> index_container_current_ring;
    
    int conti = 0; 	//for accelerate least_square fitting 

    std::vector<Kernel::Point_3> init_points;
    init_points.reserve (6);
          
    Neighbor_search search(*tree, query, 6, 0, true, tr_dist);
    for (Neighbor_search::iterator nit = search.begin(); nit != search.end(); ++ nit)
    {
      index_container.insert (nit->first);
      index_container_former_ring.push_back (nit->first);
      init_points.push_back (points_item->point_set()->point (nit->first));
    }

    Kernel::Plane_3 optimal_plane;
    CGAL::linear_least_squares_fitting_3(init_points.begin(),
                                         init_points.end(),
                                         optimal_plane,
                                         CGAL::Dimension_tag<0>());
    
    Kernel::Vector_3 plane_normal = optimal_plane.orthogonal_vector();
    plane_normal = plane_normal / std::sqrt(plane_normal * plane_normal);

    std::vector<Point_set::Index> neighbors;
    
    bool propagation = false;
    do
    {
      propagation = false;

      for (std::vector<Point_set::Index>::iterator icfrit
             = index_container_former_ring.begin();
           icfrit != index_container_former_ring.end(); ++ icfrit)
      {
        Point_set::Index point_index = *icfrit;

        neighbors.clear();
        Sphere fs (points_item->point_set()->point(point_index),
                   cluster_epsilon, 0, tree->traits());
        tree->search (std::back_inserter (neighbors), fs);
                
        for (std::size_t nb = 0; nb < neighbors.size(); ++ nb)
        {
          Point_set::Index neighbor_index = neighbors[nb];
                    
          if (index_container.find(neighbor_index) != index_container.end())
            continue;

          const Kernel::Point_3& neighbor = points_item->point_set()->point(neighbor_index);
          double distance = CGAL::squared_distance(neighbor, optimal_plane);

          if (distance > epsilon * epsilon)
            continue;

          if (points_item->point_set()->has_normal_map())
          {
            Kernel::Vector_3 normal = points_item->point_set()->normal (neighbor_index);
            normal = normal / std::sqrt (normal * normal);
            if (std::fabs (normal * plane_normal) < cos_threshold)
              continue;
          }
          
          index_container.insert (neighbor_index);
          propagation = true;
          index_container_current_ring.insert(neighbor_index);
        }
      }

      //update containers
      index_container_former_ring.clear();
      index_container_former_ring.reserve (index_container_current_ring.size());

      for (std::set<Point_set::Index>::iterator lit = index_container_current_ring.begin();
           lit != index_container_current_ring.end(); ++lit)
        index_container_former_ring.push_back(*lit);

      index_container_current_ring.clear();

      conti++;
      if (index_container.size() < 5)
        continue;
          
      if ((conti < 10) || (conti<50 && conti % 10 == 0) || (conti>50 && conti % 500 == 0))
      {
        std::list<Kernel::Point_3> listp;
        for (std::set<Point_set::Index>::iterator icit = index_container.begin();
             icit != index_container.end(); ++ icit)
          listp.push_back(points_item->point_set()->point (*icit));

        Kernel::Plane_3 reajusted_plane;
        CGAL::linear_least_squares_fitting_3(listp.begin(),
                                             listp.end(),
                                             reajusted_plane,
                                             CGAL::Dimension_tag<0>());
        optimal_plane = reajusted_plane;
        plane_normal = optimal_plane.orthogonal_vector();
        plane_normal = plane_normal / std::sqrt(plane_normal * plane_normal);
      }
    }
    while (propagation);

    std::vector<Point_set::Index> unselected, selected;

    for(Point_set::iterator it = points_item->point_set()->begin ();
	it != points_item->point_set()->end(); ++ it)
      if (index_container.find (*it) != index_container.end())
        selected.push_back (*it);
      else
        unselected.push_back (*it);

    for (std::size_t i = 0; i < unselected.size(); ++ i)
      *(points_item->point_set()->begin() + i) = unselected[i];
    for (std::size_t i = 0; i < selected.size(); ++ i)
      *(points_item->point_set()->begin() + (unselected.size() + i)) = selected[i];

    if (selected.empty ())
      {
	points_item->point_set()->unselect_all();
      }
    else
      {
	points_item->point_set()->set_first_selected
	  (points_item->point_set()->begin() + unselected.size());
      }
    points_item->invalidateOpenGLBuffers();
    points_item->itemChanged();
  }

};

using namespace CGAL::Three;
class Polyhedron_demo_point_set_selection_plugin :
  public QObject,
  public Polyhedron_demo_plugin_helper
{
  Q_OBJECT
    Q_INTERFACES(CGAL::Three::Polyhedron_demo_plugin_interface)
    Q_PLUGIN_METADATA(IID "com.geometryfactory.PolyhedronDemo.PluginInterface/1.0")
public:
  bool applicable(QAction*) const { 
      return qobject_cast<Scene_points_with_normal_item*>(scene->item(scene->mainSelectionIndex()));
  }
  void print_message(QString message) { CGAL::Three::Three::information(message); }
  QList<QAction*> actions() const { return QList<QAction*>() << actionPointSetSelection; }

  void init(QMainWindow* mainWindow, CGAL::Three::Scene_interface* scene_interface, Messages_interface* m) {
    mw = mainWindow;
    scene = scene_interface;
    messages = m;
    rg_epsilon = -1;
    rg_cluster_epsilon = -1;
    rg_normal_threshold = 20;
    actionPointSetSelection = new QAction(tr("Selection"), mw);
    actionPointSetSelection->setObjectName("actionPointSetSelection");
    connect(actionPointSetSelection, SIGNAL(triggered()), this, SLOT(selection_action()));

    dock_widget = new QDockWidget("Point Set Selection", mw);
    dock_widget->setVisible(false);

    ui_widget.setupUi(dock_widget);
    addDockWidget(dock_widget);

    connect(ui_widget.region, SIGNAL(toggled(bool)), this, SLOT(set_region_parameters(bool)));
    
    // Fill actions of menu
    ui_widget.menu->setMenu (new QMenu("Point Set Selection Menu", ui_widget.menu));
    QAction* select_all = ui_widget.menu->menu()->addAction ("Select all points");
    connect(select_all,  SIGNAL(triggered()), this, SLOT(on_Select_all_button_clicked()));
    QAction* clear = ui_widget.menu->menu()->addAction ("Clear selection");
    connect(clear,  SIGNAL(triggered()), this, SLOT(on_Clear_button_clicked()));
    QAction* invert = ui_widget.menu->menu()->addAction ("Invert selection");
    connect(invert,  SIGNAL(triggered()), this, SLOT(on_Invert_selection_button_clicked()));
    QAction* erase = ui_widget.menu->menu()->addAction ("Erase selected points");
    connect(erase,  SIGNAL(triggered()), this, SLOT(on_Erase_selected_points_button_clicked()));
    QAction* create = ui_widget.menu->menu()->addAction ("Create point set item from selected points");
    connect(create, SIGNAL(triggered()), this, SLOT(on_Create_point_set_item_button_clicked()));

    ui_widget.menu->menu()->addSection ("Box selection");
    add_box = ui_widget.menu->menu()->addAction ("Apply");
    connect(add_box, SIGNAL(triggered()), this, SLOT(select_points()));
    add_box->setEnabled(false);

    connect(ui_widget.box, SIGNAL(toggled(bool)), 
            this, SLOT(on_box_toggled(bool)));
    connect(ui_widget.helpButton, &QAbstractButton::clicked,
    [this](){
      QMessageBox::information(dock_widget, QString("Help"),
                               QString("SHIFT + Left Click : selection\n"
                                       "CONTROL + Left Click : print coordinates of point under cursor."));
    }
  );

    visualizer = NULL;
    edit_box = NULL;
    shift_pressing = false;
    ctrl_pressing = false;
    
    CGAL::QGLViewer* viewer = *CGAL::QGLViewer::QGLViewerPool().begin();
    viewer->installEventFilter(this);
    mainWindow->installEventFilter(this);


  }
  virtual void closure()
  {
    dock_widget->hide();
  }

protected:

  bool eventFilter(QObject *, QEvent *event) {
    static QImage background;
    if (dock_widget->isHidden() || !(dock_widget->isActiveWindow()) || ui_widget.box->isChecked())
      return false;
    
    Scene_points_with_normal_item* point_set_item
      = qobject_cast<Scene_points_with_normal_item*>(scene->item(scene->mainSelectionIndex()));
    if(!point_set_item) {
      return false; 
    }
      
    if(event->type() == QEvent::KeyPress || event->type() == QEvent::KeyRelease)
    {
      QKeyEvent *keyEvent = static_cast<QKeyEvent*>(event);
      Qt::KeyboardModifiers modifiers = keyEvent->modifiers();

      shift_pressing = modifiers.testFlag(Qt::ShiftModifier);
      ctrl_pressing = modifiers.testFlag(Qt::ControlModifier);
    }

    // mouse events
    if(shift_pressing && event->type() == QEvent::MouseButtonPress)
      {
      background = static_cast<CGAL::Three::Viewer_interface*>(*CGAL::QGLViewer::QGLViewerPool().begin())->grabFramebuffer();
	QMouseEvent *mouseEvent = static_cast<QMouseEvent*>(event);
	// Start selection
	if (mouseEvent->button() == Qt::LeftButton)
        {
          // Region growing
          if (ui_widget.region->isChecked())
          {
            QApplication::setOverrideCursor(Qt::WaitCursor);

            CGAL::QGLViewer* viewer = *CGAL::QGLViewer::QGLViewerPool().begin();
            bool found = false;
            QPoint pixel(mouseEvent->pos().x(),
                         viewer->camera()->screenHeight() - 1 - mouseEvent->pos().y());
            
            CGAL::qglviewer::Vec point = viewer->camera()->pointUnderPixel(mouseEvent->pos(),
                                                                     found);
            if(!found)
            {
              QApplication::restoreOverrideCursor();
              return false;
            }
            const CGAL::qglviewer::Vec offset = static_cast<CGAL::Three::Viewer_interface*>(viewer)->offset();
            point = point - offset;
            
            neighborhood.point_set (point_set_item).grow_region
              (Kernel::Point_3 (point.x, point.y, point.z),
               rg_epsilon, rg_cluster_epsilon, rg_normal_threshold);
            
            QApplication::restoreOverrideCursor();
            return true;
          }
          // Start standard selection
          else if (!visualizer)
          {
	    QApplication::setOverrideCursor(Qt::CrossCursor);
            CGAL::QGLViewer* viewer = *CGAL::QGLViewer::QGLViewerPool().begin();
            if (viewer->camera()->frame()->isSpinning())
              viewer->camera()->frame()->stopSpinning();
	
            visualizer = new Selection_visualizer(ui_widget.rectangle->isChecked(),
                                                                  point_set_item->bbox());

            visualizer->sample_mouse_path(background);
            return true;
          }
        }
	// Cancel selection
	else if (mouseEvent->button() == Qt::RightButton && visualizer)
	  {
	    visualizer = NULL;
	    QApplication::restoreOverrideCursor();
            return true;
	  }
      }
      // Expand/reduce selection
    else if (shift_pressing && event->type() == QEvent::Wheel)
      {
        QApplication::setOverrideCursor(Qt::WaitCursor);
        QWheelEvent *mouseEvent = static_cast<QWheelEvent*>(event);
        int steps = mouseEvent->delta() / 120;
        if (steps > 0)
          neighborhood.point_set (point_set_item).expand();
        else
          neighborhood.point_set (point_set_item).reduce();
        QApplication::restoreOverrideCursor();
      }
    // End selection
    else if (event->type() == QEvent::MouseButtonRelease && visualizer)
      {
	visualizer->apply_path();
	select_points();
	visualizer = NULL;
	QApplication::restoreOverrideCursor();
        static_cast<CGAL::Three::Viewer_interface*>(*CGAL::QGLViewer::QGLViewerPool().begin())->set2DSelectionMode(false);
	return true;
      }
    // Update selection
    else if (event->type() == QEvent::MouseMove && visualizer)
      {
        visualizer->sample_mouse_path(background);
	return true;
      }
    //Position request
    else if(ctrl_pressing && event->type() == QEvent::MouseButtonPress)
    {
      QMouseEvent* m_e = static_cast<QMouseEvent*>(event);
      Scene_points_with_normal_item* item =
          qobject_cast<Scene_points_with_normal_item*>(scene->item(scene->mainSelectionIndex()));
      if(!item)
        return false;
      CGAL::QGLViewer* viewer = *CGAL::QGLViewer::QGLViewerPool().begin();
      bool found = false;
      QPoint pixel(m_e->pos().x(),
             viewer->camera()->screenHeight()-1-m_e->pos().y());
      CGAL::qglviewer::Vec point = viewer->camera()->pointUnderPixel(m_e->pos(),
                                                               found);
      if(!found)
        return false;
      typedef Kernel::Point_3 Point;
      typedef CGAL::Search_traits_3<Kernel> TreeTraits;
      typedef CGAL::Orthogonal_k_neighbor_search<TreeTraits> Neighbor_search;
      typedef Neighbor_search::Tree Tree;
      Tree tree(item->point_set()->points().begin(), item->point_set()->points().end());

      const CGAL::qglviewer::Vec offset = static_cast<CGAL::Three::Viewer_interface*>(viewer)->offset();
      point = point - offset;
      
      Neighbor_search search(tree, Point(point.x, point.y, point.z), 1);
      Point res = search.begin()->first;
      CGAL::Three::Three::information(QString("Selected point : (%1, %2, %3)").arg(res.x()).arg(res.y()).arg(res.z()));
    }
    return false;
  }

protected Q_SLOTS:
  void select_points()
  {
    Scene_points_with_normal_item* point_set_item = getSelectedItem<Scene_points_with_normal_item>();
    if(!point_set_item)
      {
	print_message("Error: no point set selected!");
	return; 
      }

    Point_set* points = point_set_item->point_set();

    if (ui_widget.new_selection->isChecked())
      points->unselect_all();
    
    CGAL::QGLViewer* viewer = *CGAL::QGLViewer::QGLViewerPool().begin();
    const CGAL::qglviewer::Vec offset = static_cast<CGAL::Three::Viewer_interface*>(viewer)->offset();

    CGAL::qglviewer::Camera* camera = viewer->camera();

    bool* selected_bitmap = new bool[points->size()]; // std::vector<bool> is not thread safe

    Selection_test selection_test (points, selected_bitmap,
                                   camera, offset, edit_box, visualizer, ui_widget);
#ifdef CGAL_LINKED_WITH_TBB
    tbb::parallel_for(tbb::blocked_range<size_t>(0, points->size()),
                      selection_test);
#else
    for (std::size_t i = 0; i < points->size(); ++ i)
      selection_test.apply(i);
#endif

    points->set_first_selected
      (std::partition (points->begin(), points->end(),
                       [&] (const Point_set::Index& idx) -> bool
                       { return !selected_bitmap[idx]; }));
    
    point_set_item->invalidateOpenGLBuffers();
    point_set_item->itemChanged();
  }

  void set_region_parameters(bool toggled)
  {
    if (!toggled)
      return;

    QMultipleInputDialog dialog ("Region Selection Parameters", mw);
    QDoubleSpinBox* epsilon = dialog.add<QDoubleSpinBox> ("Epsilon: ");
    epsilon->setRange (0.00001, 1000000.);
    epsilon->setDecimals (5);
    if (rg_epsilon < 0.)
      rg_epsilon = (std::max)(0.00001, 0.005 * scene->len_diagonal());
    epsilon->setValue (rg_epsilon);
    
    QDoubleSpinBox* cluster_epsilon = dialog.add<QDoubleSpinBox> ("Cluster epsilon: ");
    cluster_epsilon->setRange (0.00001, 1000000.);
    cluster_epsilon->setDecimals (5);
    if (rg_cluster_epsilon < 0.)
      rg_cluster_epsilon = (std::max)(0.00001, 0.03 * scene->len_diagonal());
    cluster_epsilon->setValue (rg_cluster_epsilon);

    QSpinBox* normal_threshold = dialog.add<QSpinBox> ("Normal threshold: ");
    normal_threshold->setRange (0, 90);
    normal_threshold->setSuffix (QString("°"));
    normal_threshold->setValue (rg_normal_threshold);
    
    if (dialog.exec())
    {
      rg_epsilon = epsilon->value();
      rg_cluster_epsilon = cluster_epsilon->value();
      rg_normal_threshold = normal_threshold->value();
    }
  }
  


public Q_SLOTS:
  void selection_action() { 
    dock_widget->show();
    dock_widget->raise();
  }
  
  // Select all
  void on_Select_all_button_clicked() {
    Scene_points_with_normal_item* point_set_item = getSelectedItem<Scene_points_with_normal_item>();
    if(!point_set_item)
      {
	print_message("Error: no point set selected!");
	return; 
      }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    point_set_item->selectAll();
    QApplication::restoreOverrideCursor();
  }
  
  // Clear selection
  void on_Clear_button_clicked() {
    Scene_points_with_normal_item* point_set_item
      = qobject_cast<Scene_points_with_normal_item*>(scene->item(scene->mainSelectionIndex()));
    if(!point_set_item) {
      print_message("Error: no point set selected!");
      return; 
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    point_set_item->resetSelection();
    QApplication::restoreOverrideCursor();
  }

  void on_Erase_selected_points_button_clicked() {
    Scene_points_with_normal_item* point_set_item
      = qobject_cast<Scene_points_with_normal_item*>(scene->item(scene->mainSelectionIndex()));
    if(!point_set_item) {
      print_message("Error: no point set selected!");
      return; 
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    point_set_item->deleteSelection();
    QApplication::restoreOverrideCursor();
  }

  void on_Invert_selection_button_clicked() {
    Scene_points_with_normal_item* point_set_item
      = qobject_cast<Scene_points_with_normal_item*>(scene->item(scene->mainSelectionIndex()));
    if(!point_set_item) {
      print_message("Error: no point set selected!");
      return; 
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    point_set_item->invertSelection();
    QApplication::restoreOverrideCursor();
  }

  void on_Create_point_set_item_button_clicked() {
    Scene_points_with_normal_item* point_set_item
      = qobject_cast<Scene_points_with_normal_item*>(scene->item(scene->mainSelectionIndex()));
    if(!point_set_item) {
      print_message("Error: no point set selected!");
      return; 
    }
    if(point_set_item->isSelectionEmpty ()) {
      print_message("Error: there is no selected point in point set item!");
      return;
    }
    QApplication::setOverrideCursor(Qt::WaitCursor);
    Scene_points_with_normal_item* new_item = new Scene_points_with_normal_item;
    new_item->point_set()->copy_properties (*(point_set_item->point_set()));
    new_item->point_set()->check_colors();

    new_item->setName(QString("%1 (selected points)").arg(point_set_item->name()));
    
    new_item->setColor(point_set_item->color());
    new_item->setRenderingMode(point_set_item->renderingMode());
    new_item->setVisible(point_set_item->visible());
    
    std::cerr << point_set_item->point_set()->info() << std::endl;
    std::cerr << new_item->point_set()->info() << std::endl;

    typedef Point_set_3<Kernel> Point_set;
    for(Point_set::iterator it = point_set_item->point_set()->first_selected ();
        it != point_set_item->point_set()->end(); ++ it)
    {
      new_item->point_set()->insert (*(point_set_item->point_set()), *it);
    }
    new_item->resetSelection();
    new_item->invalidateOpenGLBuffers();

    scene->addItem(new_item);
    QApplication::restoreOverrideCursor();
 }

  void on_box_toggled (bool toggle)
  {
    if(toggle)
    {
      CGAL::QGLViewer* viewer = *CGAL::QGLViewer::QGLViewerPool().begin();
      qobject_cast<Viewer_interface*>(viewer)->set2DSelectionMode(false);
      edit_box = new Scene_edit_box_item(scene);
      edit_box->setRenderingMode(Wireframe);
      edit_box->setName("Selection Box");
      connect(edit_box, &Scene_edit_box_item::aboutToBeDestroyed,
              this, &Polyhedron_demo_point_set_selection_plugin::reset_editbox);
      scene->addItem(edit_box);
      viewer->installEventFilter(edit_box);
      add_box->setEnabled(true);
    }
    else
    {
      if(edit_box)
        scene->erase(scene->item_id(edit_box));
      edit_box = NULL;
      add_box->setEnabled(false);
    }


  }


  void reset_editbox()
  {
    edit_box = NULL;
  }

private:
  Messages_interface* messages;
  QAction* actionPointSetSelection;

  QDockWidget* dock_widget;
  QAction* add_box;
  
  Ui::PointSetSelection ui_widget;
  Selection_visualizer* visualizer;
  Neighborhood neighborhood;
  bool shift_pressing;
  bool ctrl_pressing;
  Scene_edit_box_item* edit_box;

  // Region growing selection
  double rg_epsilon;
  double rg_cluster_epsilon;
  unsigned int rg_normal_threshold;
}; // end Polyhedron_demo_point_set_selection_plugin

//Q_EXPORT_PLUGIN2(Polyhedron_demo_point_set_selection_plugin, Polyhedron_demo_point_set_selection_plugin)

#include "Point_set_selection_plugin.moc"
