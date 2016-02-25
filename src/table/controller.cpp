/*****************************************************************************
* Copyright 2015-2016 Alexander Barthel albar965@mailbox.org
*
* This program is free software: you can redistribute it and/or modify
* it under the terms of the GNU General Public License as published by
* the Free Software Foundation, either version 3 of the License, or
* (at your option) any later version.
*
* This program is distributed in the hope that it will be useful,
* but WITHOUT ANY WARRANTY; without even the implied warranty of
* MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
* GNU General Public License for more details.
*
* You should have received a copy of the GNU General Public License
* along with this program.  If not, see <http://www.gnu.org/licenses/>.
*****************************************************************************/

#include "table/controller.h"

#include "geo/calculations.h"
#include "sql/sqldatabase.h"
#include "sql/sqlquery.h"
#include "settings/settings.h"
#include "column.h"
#include "sqlproxymodel.h"
#include "table/sqlmodel.h"
#include "table/columnlist.h"

#include <QTableView>
#include <QHeaderView>
#include <QSettings>
#include <QSpinBox>
#include <QSortFilterProxyModel>
#include <QApplication>
#include <QEventLoop>

#include <geo/rect.h>

using atools::sql::SqlQuery;
using atools::sql::SqlDatabase;

Controller::Controller(QWidget *parent, atools::sql::SqlDatabase *sqlDb, ColumnList *cols,
                       QTableView *tableView)
  : parentWidget(parent), db(sqlDb), view(tableView), columns(cols)
{
}

Controller::~Controller()
{
  clearModel();
}

void Controller::clearModel()
{
  if(!isGrouped())
    saveViewState();

  viewSetModel(nullptr);

  if(proxyModel != nullptr)
    proxyModel->clear();
  delete proxyModel;
  proxyModel = nullptr;

  if(model != nullptr)
    model->clear();
  delete model;
  model = nullptr;
}

void Controller::filterIncluding(const QModelIndex& index)
{
  Q_ASSERT(model != nullptr);
  model->filterIncluding(toS(index));
}

void Controller::filterExcluding(const QModelIndex& index)
{
  Q_ASSERT(model != nullptr);
  model->filterExcluding(toS(index));
}

void Controller::filterByLineEdit(const Column *col, const QString& text)
{
  Q_ASSERT(model != nullptr);
  model->filter(col, text);
}

void Controller::filterBySpinBox(const Column *col, int value)
{
  Q_ASSERT(model != nullptr);
  model->filter(col, value);
}

void Controller::filterByMinMaxSpinBox(const Column *col, int minValue, int maxValue)
{
  Q_ASSERT(model != nullptr);
  QVariant minVal(minValue), maxVal(maxValue);
  if(col->getMinSpinBoxWidget()->value() == col->getMinSpinBoxWidget()->minimum())
    minVal = QVariant(QVariant::Int);

  if(col->getMaxSpinBoxWidget()->value() == col->getMaxSpinBoxWidget()->maximum())
    maxVal = QVariant(QVariant::Int);

  model->filter(col, minVal, maxVal);
}

void Controller::filterByCheckbox(const Column *col, int state, bool triState)
{
  Q_ASSERT(model != nullptr);

  if(triState)
  {
    Qt::CheckState s = static_cast<Qt::CheckState>(state);
    switch(s)
    {
      case Qt::Unchecked:
        model->filter(col, 0);
        break;
      case Qt::PartiallyChecked:
        model->filter(col, QVariant(QVariant::Int));
        break;
      case Qt::Checked:
        model->filter(col, 1);
        break;
    }
  }
  else
    model->filter(col, state == Qt::Checked ? 1 : QVariant(QVariant::Int));
}

void Controller::filterByComboBox(const Column *col, int value, bool noFilter)
{
  Q_ASSERT(model != nullptr);
  if(noFilter)
    // Index 0 for combo box means here: no filter, so remove it
    model->filter(col, QVariant(QVariant::Int));
  else
    model->filter(col, value);
}

void Controller::filterByDistance(const atools::geo::Pos& center, sqlproxymodel::SearchDirection dir,
                                  int minDistance, int maxDistance)
{
  if(center.isValid())
  {
    currentDistanceCenter = center;
    atools::geo::Rect rect(center, atools::geo::nmToMeter(maxDistance));

    bool proxyWasNull = false;
    if(proxyModel == nullptr)
    {
      proxyWasNull = true;
      // Controller takes ownership
      proxyModel = new SqlProxyModel(this, model);
      proxyModel->setDynamicSortFilter(true);
      proxyModel->setSourceModel(model);

      viewSetModel(proxyModel);
    }

    proxyModel->setDistanceFilter(center, dir, minDistance, maxDistance);
    model->filterByBoundingRect(rect);

    if(proxyWasNull)
    {
      proxyModel->sort(0, Qt::DescendingOrder);
      model->setSort("distance", Qt::DescendingOrder);
      model->fillHeaderData();
      view->reset();
      processViewColumns();
    }
  }
  else
  {
    currentDistanceCenter = atools::geo::Pos();

    viewSetModel(model);

    if(proxyModel != nullptr)
    {
      proxyModel->clear();
      delete proxyModel;
      proxyModel = nullptr;
    }

    model->filterByBoundingRect(atools::geo::Rect());
    model->fillHeaderData();
    processViewColumns();
  }
}

void Controller::filterByDistanceUpdate(sqlproxymodel::SearchDirection dir, int minDistance, int maxDistance)
{
  if(currentDistanceCenter.isValid())
  {
    atools::geo::Rect rect(currentDistanceCenter, atools::geo::nmToMeter(maxDistance));

    proxyModel->setDistanceFilter(currentDistanceCenter, dir, minDistance, maxDistance);
    model->filterByBoundingRect(rect);
  }
}

void Controller::filterOperator(bool useAnd)
{
  Q_ASSERT(model != nullptr);
  if(useAnd)
    model->filterOperator("and");
  else
    model->filterOperator("or");
}

void Controller::viewSetModel(QAbstractItemModel *newModel)
{
  QItemSelectionModel *m = view->selectionModel();
  view->setModel(newModel);
  delete m;
}

void Controller::groupByColumn(const QModelIndex& index)
{
  Q_ASSERT(model != nullptr);
  Q_ASSERT(columns != nullptr);
  Q_ASSERT(!isGrouped());

  saveViewState();

  columns->clearWidgets();
  // Disable all search widgets except the one for the group by column
  columns->enableWidgets(false, {model->record().fieldName(index.column())});

  model->groupByColumn(index);
  processViewColumns();
  // No column order or size stored for grouped views
  view->resizeColumnsToContents();
}

void Controller::ungroup()
{
  Q_ASSERT(model != nullptr);
  Q_ASSERT(columns != nullptr);
  columns->clearWidgets();
  columns->enableWidgets(true);

  model->ungroup();
  processViewColumns();

  if(!isGrouped())
    restoreViewState();
}

void Controller::selectAll()
{
  Q_ASSERT(view->selectionModel() != nullptr);
  return view->selectAll();
}

const QItemSelection Controller::getSelection() const
{
  Q_ASSERT(view->selectionModel() != nullptr);
  return view->selectionModel()->selection();
}

int Controller::getVisibleRowCount() const
{
  if(proxyModel != nullptr)
    return proxyModel->rowCount();
  else if(model != nullptr)
    return model->rowCount();

  return 0;
}

int Controller::getTotalRowCount() const
{
  // TODO not accurate when filtering with proxy
  if(model != nullptr)
    return model->getTotalRowCount();
  else
    return 0;
}

bool Controller::isColumnVisibleInView(int physicalIndex) const
{
  Q_ASSERT(model != nullptr);
  return view->columnWidth(physicalIndex) > view->horizontalHeader()->minimumSectionSize() + 1;
}

int Controller::getColumnVisualIndex(int physicalIndex) const
{
  Q_ASSERT(model != nullptr);
  return view->horizontalHeader()->visualIndex(physicalIndex);
}

const Column *Controller::getColumn(const QString& colName) const
{
  Q_ASSERT(model != nullptr);
  return model->getColumnModel(colName);
}

const Column *Controller::getColumn(int physicalIndex) const
{
  Q_ASSERT(model != nullptr);
  return model->getColumnModel(physicalIndex);
}

void Controller::resetView()
{
  Q_ASSERT(model != nullptr);
  if(columns != nullptr)
  {
    columns->clearWidgets();
    columns->enableWidgets(true);
  }

  // Reorder columns to match model order
  QHeaderView *header = view->horizontalHeader();
  for(int i = 0; i < header->count(); ++i)
    header->moveSection(header->visualIndex(i), i);

  model->reset();

  processViewColumns();

  view->resizeColumnsToContents();
  saveViewState();
}

void Controller::resetSearch()
{
  if(columns != nullptr)
    columns->clearWidgets();

  currentDistanceCenter = atools::geo::Pos();
  if(proxyModel != nullptr)
    proxyModel->clearDistanceFilter();

  if(model != nullptr)
    model->resetSearch();
}

QString Controller::getCurrentSqlQuery() const
{
  Q_ASSERT(model != nullptr);
  return model->getCurrentSqlQuery();
}

QModelIndex Controller::getModelIndexAt(const QPoint& pos) const
{
  return view->indexAt(pos);
}

QString Controller::getHeaderNameAt(const QModelIndex& index) const
{
  Q_ASSERT(model != nullptr);
  return model->headerData(index.column(), Qt::Horizontal).toString();
}

QString Controller::getFieldDataAt(const QModelIndex& index) const
{
  Q_ASSERT(model != nullptr);
  return model->getFormattedFieldData(toS(index)).toString();
}

QModelIndex Controller::toS(const QModelIndex& index) const
{
  if(proxyModel != nullptr)
    return proxyModel->mapToSource(index);
  else
    return index;
}

QModelIndex Controller::fromS(const QModelIndex& index) const
{
  if(proxyModel != nullptr)
    return proxyModel->mapFromSource(index);
  else
    return index;
}

int Controller::getIdForRow(const QModelIndex& index)
{
  if(index.isValid())
    return model->getRawData(toS(index).row(), "airport_id").toInt();
  else
    return -1;
}

bool Controller::isGrouped() const
{
  if(model != nullptr)
    return model->isGrouped();
  else
    return false;
}

void Controller::processViewColumns()
{
  Q_ASSERT(model != nullptr);
  Q_ASSERT(columns != nullptr);

  const Column *sort = nullptr;
  QSqlRecord rec = model->record();
  int cnt = rec.count();
  for(int i = 0; i < cnt; ++i)
  {
    QString field = rec.fieldName(i);
    const Column *cd = columns->getColumn(field);

    if(!currentDistanceCenter.isValid() && cd->isVirtualCol())
    {
      qDebug() << "hide" << i << "name" << field;
      view->hideColumn(i);
    }
    else
    // Hide or show column
    if(cd->isHiddenCol())
    {
      qDebug() << "hide" << i << "name" << field;
      view->hideColumn(i);
    }
    else
    {
      qDebug() << "show" << i << "name" << field;
      view->showColumn(i);
    }

    // Set sort column
    if(model->getSortColumn().isEmpty())
    {
      qDebug() << "default sort" << i << "name" << field;
      if(cd->isDefaultSortCol())
      {
        qDebug() << "default sort" << i << "name" << field;
        view->sortByColumn(i, cd->getDefaultSortOrder());
        sort = cd;
      }
    }
    else if(field == model->getSortColumn())
    {
      qDebug() << "sort" << i << "name" << field;
      view->sortByColumn(i, model->getSortOrder());
      sort = cd;
    }
  }

  const Column *c = columns->getDefaultSortColumn();
  int idx = rec.indexOf(c->getColumnName());
  qDebug() << "sort to default " << idx << " name" << c->getColumnName();
  if(sort == nullptr)
    view->sortByColumn(idx, c->getDefaultSortOrder());
  else
  {
    if(sort->isHiddenCol())
      view->sortByColumn(idx, c->getDefaultSortOrder());
    else if(!currentDistanceCenter.isValid())
      if(sort->isVirtualCol())
        view->sortByColumn(idx, c->getDefaultSortOrder());
  }
}

void Controller::prepareModel()
{
  model = new SqlModel(parentWidget, db, columns);

  QItemSelectionModel *m = view->selectionModel();
  view->setModel(model);
  delete m;

  model->fillHeaderData();
  processViewColumns();
  if(!isGrouped())
    restoreViewState();
}

void Controller::saveViewState()
{
  viewState = view->horizontalHeader()->saveState();
}

void Controller::restoreViewState()
{
  if(!viewState.isEmpty())
    view->horizontalHeader()->restoreState(viewState);
}

void Controller::loadAllRowsForRectQuery()
{
  Q_ASSERT(model != nullptr);

  if(proxyModel != nullptr)
  {
    QGuiApplication::setOverrideCursor(Qt::WaitCursor);
    model->resetSqlQuery();
    proxyModel->invalidate();
    while(model->canFetchMore())
      model->fetchMore(QModelIndex());
    QGuiApplication::restoreOverrideCursor();
  }
}

void Controller::loadAllRows()
{
  Q_ASSERT(model != nullptr);

  QGuiApplication::setOverrideCursor(Qt::WaitCursor);

  if(proxyModel != nullptr)
  {
    model->resetSqlQuery();
    proxyModel->invalidate();
  }

  while(model->canFetchMore())
    model->fetchMore(QModelIndex());

  QGuiApplication::restoreOverrideCursor();
}

void Controller::connectModelReset(std::function<void(void)> func)
{
  if(model != nullptr)
    connect(model, &SqlModel::modelReset, func);
}

void Controller::connectFetchedMore(std::function<void(void)> func)
{
  if(model != nullptr)
    connect(model, &SqlModel::fetchedMore, func);
}

QVector<const Column *> Controller::getCurrentColumns() const
{
  Q_ASSERT(model != nullptr);
  Q_ASSERT(columns != nullptr);

  QVector<const Column *> cols;
  QSqlRecord rec = model->record();
  cols.clear();
  for(int i = 0; i < rec.count(); ++i)
    cols.append(columns->getColumn(rec.fieldName(i)));
  return cols;
}

QString Controller::formatModelData(const QString& col, const QVariant& var) const
{
  Q_ASSERT(model != nullptr);
  return model->formatValue(col, var);
}

QVariantList Controller::getFormattedModelData(int row) const
{
  Q_ASSERT(model != nullptr);
  return model->getFormattedRowData(row);
}

QVariantList Controller::getRawModelData(int row) const
{
  Q_ASSERT(model != nullptr);
  return model->getRawRowData(row);
}

QStringList Controller::getRawModelColumns() const
{
  Q_ASSERT(model != nullptr);
  return model->getRawColumns();
}

QString Controller::getSortColumn() const
{
  Q_ASSERT(model != nullptr);
  return model->getSortColumn();
}
